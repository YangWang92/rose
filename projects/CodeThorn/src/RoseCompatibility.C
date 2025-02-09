
#include <sage3basic.h>
#include <sageInterface.h>
#include <sageGeneric.h>
#include <VariableIdMapping.h>

#include <tuple>
#include <unordered_set>

#include "CodeThornLib.h"
#include "FunctionIdMapping.h"
#include "RoseCompatibility.h"
#include "ClassHierarchyAnalysis.h"


namespace si = SageInterface;
namespace ct = CodeThorn;


namespace
{
  static constexpr bool firstDecisiveComparison = false; // syntactic sugar tag

  std::string typeNameOf(const SgClassDeclaration* dcl)
  {
    return SG_DEREF(dcl).get_name();
  }

  std::string typeNameOf(const SgClassDefinition& def)
  {
    return typeNameOf(def.get_declaration());
  }

  struct ExcludeTemplates
  {
    void handle(SgTemplateClassDeclaration&)          {}
    void handle(SgTemplateClassDefinition&)           {}
    void handle(SgTemplateFunctionDeclaration&)       {}
    void handle(SgTemplateFunctionDefinition&)        {}
    void handle(SgTemplateMemberFunctionDeclaration&) {}
    void handle(SgTemplateVariableDeclaration&)       {}
    //~ void handle(SgTemplateTypedefDeclaration&)        {} // may need some handling
  };

  bool isVirtual(const SgMemberFunctionDeclaration& dcl)
  {
    return dcl.get_functionModifier().isVirtual();
  }


  struct NodeCollector : ExcludeTemplates
  {
      typedef std::unordered_set<SgNode*> NodeTracker;

      NodeCollector( NodeTracker& visited,
                     ct::ClassAnalysis& classes,
                     ct::CastAnalysis& casts,
                     const ct::RoseCompatibilityBridge& compat
                   )
      : visitedNodes(&visited), allClasses(&classes), allCasts(&casts), rcb(&compat), currentClass(nullptr)
      {}

      void descend(SgNode& n, ct::ClassAnalysis::value_type* cls = nullptr);

      using ExcludeTemplates::handle;

      void handle(SgNode& n)                  { descend(n); }

      // classes
      void handle(SgClassDefinition& n)
      {
        SgClassDeclaration&     decl   = SG_DEREF(n.get_declaration());
        SgDeclarationStatement* defdcl = decl.get_definingDeclaration();
        SgClassDeclaration&     clsdef = SG_DEREF(isSgClassDeclaration(defdcl));

        // exclude secondary declarations
        if (clsdef.get_definition() != &n)
          return;

        auto emplaced = allClasses->emplace(&n, ct::ClassData{});

        ROSE_ASSERT(emplaced.second);
        descend(n, &*emplaced.first);
      }

      void handle(SgMemberFunctionDeclaration& n)
      {
        if (currentClass && isVirtual(n))
          currentClass->second.virtualFunctions().emplace_back(rcb->functionId(&n));

        descend(n);
      }

      void handle(SgVariableDeclaration& n)
      {
        if (currentClass && !si::isStatic(&n))
          currentClass->second.dataMembers().emplace_back(rcb->variableId(&n));

        descend(n);
      }


      // casts
      void handle(SgCastExp& n)
      {
        SgExpression& expr = SG_DEREF(n.get_operand());

        // store cast types and cast expressions
        (*allCasts)[ct::CastDesc{expr.get_type(), n.get_type()}].push_back(&n);

        descend(n);
      }

      NodeCollector withClass(ct::ClassAnalysis::value_type* cls)
      {
        NodeCollector tmp{*this};

        tmp.currentClass = cls;
        return tmp;
      }

    private:
      NodeTracker*                       visitedNodes;
      ct::ClassAnalysis*                 allClasses;
      ct::CastAnalysis*                  allCasts;
      const ct::RoseCompatibilityBridge* rcb;
      ct::ClassAnalysis::value_type*     currentClass;
  };

  template <class SetT, class ElemT>
  inline
  bool alreadyProcessed(SetT& s, const ElemT& e)
  {
    return !s.insert(e).second;
  }

  void NodeCollector::descend(SgNode& n, ct::ClassAnalysis::value_type* cls)
  {
    if (alreadyProcessed(*visitedNodes, &n))
      return;

    sg::traverseChildren(this->withClass(cls), n);
  }

  template <class V>
  int cmpValue(const V& lhs, const V& rhs)
  {
    enum cmpresult : int { LT = -1, EQ = 0, GT = 1 };

    if (lhs < rhs) return LT;
    if (rhs < lhs) return GT;

    return EQ;
  }

  int cmpArrayLength(const SgArrayType& lhs, const SgArrayType& rhs)
  {
    int               cmpres = 0;
    ct::AbstractValue lhsval = evaluateExpressionWithEmptyState(lhs.get_index());
    ct::AbstractValue rhsval = evaluateExpressionWithEmptyState(rhs.get_index());

    // \todo check correctness of array length comparison
    firstDecisiveComparison
    || (cmpres = cmpValue(lhsval.isConstInt(), rhsval.isConstInt()))
    || (!lhsval.isConstInt()) // if not const, then assume that types are equal
    || (cmpres = cmpValue(lhsval.getIntValue(), rhsval.getIntValue()))
    ;

    return cmpres;
  }


  template <class SageModifier>
  int cmpModifier(const SageModifier& lhs, const SageModifier& rhs)
  {
    return cmpValue(lhs.get_modifier(), rhs.get_modifier());
  }

  int cmpModifier(const SgTypeModifier& lhs, const SgTypeModifier& rhs)
  {
    int cmpres = 0;

    firstDecisiveComparison
    || (cmpres = cmpValue(lhs.get_modifierVector(), rhs.get_modifierVector()))
    || (cmpres = cmpModifier(lhs.get_constVolatileModifier(), rhs.get_constVolatileModifier()))
    || (cmpres = cmpModifier(lhs.get_elaboratedTypeModifier(), rhs.get_elaboratedTypeModifier()))
    || (cmpres = cmpModifier(lhs.get_upcModifier(), rhs.get_upcModifier()))
    ;

    return cmpres;
  }


  /// type comparator
  struct TypeComparator : sg::DispatchHandler<int>
  {
      using base = sg::DispatchHandler<int>;

      explicit
      TypeComparator( const SgType& rhs_type,
                      bool arrayDecay = false,
                      bool ignoreFunctionReturn = false
                    )
      : base(),
        rhs(rhs_type),
        withArrayToPointerDecay(arrayDecay),
        ignoreFunctionReturnType(ignoreFunctionReturn)
      {}

      ~TypeComparator() = default;

      /// unreachable base case (non type nodes) => error
      void handle2(const SgNode& n, const SgNode&) { SG_UNEXPECTED_NODE(n); }

      // base case
      //   if we get here than the types must be different
      //   ==> we need separate handlers for non-shared types
      void handle2(const SgType& lhs, const SgType& rhs)
      {
        res = cmpValue(&lhs, &rhs);
      }

      void handle2(const SgNamedType& lhs, const SgNamedType& rhs)
      {
        const SgDeclarationStatement& lhsdcl = SG_DEREF(lhs.get_declaration());
        const SgDeclarationStatement& rhsdcl = SG_DEREF(rhs.get_declaration());
        const SgDeclarationStatement* lhskey = lhsdcl.get_firstNondefiningDeclaration();
        const SgDeclarationStatement* rhskey = rhsdcl.get_firstNondefiningDeclaration();

        res = cmpValue(lhskey, rhskey);
      }

      void handle2(const SgPointerType& lhs, const SgPointerType& rhs)
      {
        res = cmpBaseTypes(lhs, rhs);
      }

      void handle2(const SgArrayType& lhs, const SgArrayType& rhs)
      {
        firstDecisiveComparison
        || (res = cmpBaseTypes(lhs, rhs))
        || (res = cmpArrayLength(lhs, rhs))
        ;
      }

      void handle2(const SgReferenceType& lhs, const SgReferenceType& rhs)
      {
        res = cmpBaseTypes(lhs, rhs);
      }

      void handle2(const SgRvalueReferenceType& lhs, const SgRvalueReferenceType& rhs)
      {
        res = cmpBaseTypes(lhs, rhs);
      }

      void handle2(const SgModifierType& lhs, const SgModifierType& rhs)
      {
        // short circuit eval to compute the result based on the first divergent element
        firstDecisiveComparison
        || (res = cmpModifier(lhs.get_typeModifier(), rhs.get_typeModifier()))
        || (res = cmpBaseTypes(lhs, rhs))
        ;
      }

      void handle2(const SgFunctionType& lhs, const SgFunctionType& rhs)
      {
        res = cmpFunctionTypes(lhs, rhs);
      }

      void handle2(const SgMemberFunctionType& lhs, const SgMemberFunctionType& rhs)
      {
        firstDecisiveComparison
        || (res = cmpValue(lhs.get_ref_qualifiers(),  rhs.get_ref_qualifiers()))
        || (res = cmpValue(lhs.get_mfunc_specifier(), rhs.get_mfunc_specifier()))
        || (res = cmpFunctionTypes(lhs, rhs, ignoreFunctionReturnType))
        ;
      }

      /// base handler, should never be reached
      void handleTypes(const SgNode& n, const SgNode& /* tag */) { SG_UNEXPECTED_NODE(n); }

      /// main handler checking if lhs and rhs dynamic types match
      /// \details
      ///   types of rhs and lhs match:
      ///     - yes: the type comparison will be continued within the
      ///            corresponding handle2 functions.
      ///     - no:  res is set according to variantT
      template <class SageType>
      void handleTypes(const SageType& lhs, const SgType& /* tag */)
      {
        // typedef types should not be skipped before they are dispatched
        ROSE_ASSERT(lhs.variantT() != V_SgTypedefType);
        ROSE_ASSERT(rhs.variantT() != V_SgTypedefType);

        //~ res = cmpValue(SageType::static_variant, rhs.variantT());
        res = cmpValue(lhs.variantT(), rhs.variantT());

        // we are done, if the comparison was decisive
        if (res) return;

        //~ handle2(lhs, SG_ASSERT_TYPE(const SageType, rhs));
        handle2(lhs, static_cast<const SageType&>(rhs));
      }

      // forwarding handler that separates types from non-types
      template <class SageNode>
      void handle(const SageNode& lhs)
      {
        handleTypes(lhs, lhs);
      }

      /// special handlers that deal with array to pointer decay (if the flag is enabled)
      /// \{
      void handle(const SgArrayType& lhs)
      {
        if (!withArrayToPointerDecay)
          handleTypes(lhs, lhs);
        else
          handleArrayDecay(lhs, rhs);
      }

      void handle(const SgPointerType& lhs)
      {
        if (!withArrayToPointerDecay)
          handleTypes(lhs, lhs);
        else
          handleArrayDecay(lhs, rhs);
      }
      /// \}

    private:

      /// type comparison with array decay
      /// \details
      ///   treats arrays equivalent to pointers
      void handleArrayDecay(const SgType& lhs, const SgType& rhs)
      {
        std::tuple<const SgType*, VariantT> lhs_decay = decayedTypeData(lhs);
        std::tuple<const SgType*, VariantT> rhs_decay = decayedTypeData(rhs);

        firstDecisiveComparison
        || (res = cmpValue(std::get<1>(lhs_decay), std::get<1>(rhs_decay)))
        || (res = cmpTypes(std::get<0>(lhs_decay), std::get<0>(rhs_decay)), false)
        ;
      }

      // returns the base of arrays and pointers and a "decayed" variantT
      std::tuple<const SgType*, VariantT>
      decayedTypeData(const SgType& n)
      {
        // arrays treated as pointers after decay
        if (const SgArrayType* at = isSgArrayType(&n))
          return std::make_pair(at->get_base_type(), V_SgPointerType);

        if (const SgPointerType* pt = isSgPointerType(&n))
          return std::make_pair(pt->get_base_type(), V_SgPointerType);

        return std::make_pair(&n, n.variantT());
      }

      // compare base types
      template <class SageType>
      int cmpBaseTypes(const SageType& lhs, const SageType& rhs)
      {
        return cmpTypes(lhs.get_base_type(), rhs.get_base_type(), false);
      }

      int cmpFunctionTypes(const SgFunctionType& lhs, const SgFunctionType& rhs, bool ignoreReturn = false);

      int cmpTypes(const SgType* lhs, const SgType* rhs);
      int cmpTypes(const SgType* lhs, const SgType* rhs, bool ptrDecay);


    public:
      static
      int compare(const SgType&, const SgType&, bool ptrDecay = false, bool ignoreFuncReturn = false);

    private:
      const SgType& rhs;
      bool          withArrayToPointerDecay  = false;
      bool          ignoreFunctionReturnType = false;
  };

  const SgType&
  skipAliasTypes(const SgType& ty)
  {
    return SG_DEREF(ty.stripType(SgType::STRIP_TYPEDEF_TYPE));
  }

  const SgType*
  skipAliasTypes(const SgType* ty)
  {
    return (ty == nullptr) ? ty : &skipAliasTypes(*ty);
  }

  const SgType&
  returnType(const SgFunctionType& ft)
  {
    return SG_DEREF(ft.get_return_type());
  }

  int
  TypeComparator::compare(const SgType& lhs, const SgType& rhs, bool ptrDecay, bool ignoreFuncReturn)
  {
    const SgType& llhs = skipAliasTypes(lhs);
    const SgType& rrhs = skipAliasTypes(rhs);

    // types are the same if the type pointers are the same
    if (&llhs == &rrhs) return 0;

    // or if they are structurally the same
    return sg::dispatch(TypeComparator{rrhs, ptrDecay, ignoreFuncReturn}, &llhs);
  }

  int
  TypeComparator::cmpTypes(const SgType* lhs, const SgType* rhs, bool ptrDecay)
  {
    return compare(SG_DEREF(lhs), SG_DEREF(rhs), ptrDecay, ignoreFunctionReturnType);
  }

  int
  TypeComparator::cmpTypes(const SgType* lhs, const SgType* rhs)
  {
    return cmpTypes(lhs, rhs, withArrayToPointerDecay);
  }

  int
  cmpParamTypes(const SgFunctionType& lhs, const SgFunctionType& rhs)
  {
    static constexpr bool withArrayDecay = true;

    const SgTypePtrList& lhsparm = lhs.get_arguments();
    const SgTypePtrList& rhsparm = rhs.get_arguments();
    size_t               i       = 0;
    size_t               N       = lhsparm.size();
    int                  cmpres  = cmpValue(N, rhsparm.size());

    while ((cmpres == 0) && (i < N))
    {
      cmpres = TypeComparator::compare(SG_DEREF(lhsparm.at(i)), SG_DEREF(rhsparm.at(i)), withArrayDecay);
      ++i;
    }

    return cmpres;
  }

  int
  TypeComparator::cmpFunctionTypes(const SgFunctionType& lhs, const SgFunctionType& rhs, bool ignoreReturn)
  {
    // since C++ cannot return arrays, both true and false should be correct
    static constexpr bool returnDecay     = true;

    int cmpres = 0;

    firstDecisiveComparison
    || (cmpres = cmpParamTypes(lhs, rhs))
    || ignoreReturn
    || (cmpres = cmpTypes(&returnType(lhs), &returnType(rhs), returnDecay))
    ;

    return cmpres;
  }
}

namespace CodeThorn
{

std::string typeNameOf(ClassKeyType key)
{
  return ::typeNameOf(SG_DEREF(key));
}


void inheritanceEdges( const SgClassDefinition* def,
                       std::function<void(const SgClassDefinition*, const SgClassDefinition*, bool)> fn
                     )
{
  const SgClassDefinition&  clazz = SG_DEREF(def);
  const SgBaseClassPtrList& bases = clazz.get_inheritances();

  for (SgBaseClass* pbase : bases)
  {
    SgBaseClass&         base   = SG_DEREF(pbase);
    const bool           direct = base.get_isDirectBaseClass();

    if (!direct) continue;

    SgClassDeclaration*  pdecl  = base.get_base_class();

    if (!pdecl)
    {
      logWarn() << "base class declaration is not available "
                << typeid(base).name()
                << std::endl;

      continue;
    }

    SgBaseClassModifier&    bmod   = SG_DEREF(base.get_baseClassModifier());
    SgClassDeclaration&     decl   = SG_DEREF(pdecl);
    SgDeclarationStatement* defdcl = decl.get_definingDeclaration();
    SgClassDeclaration&     clsdef = SG_DEREF(isSgClassDeclaration(defdcl));

    fn(def, clsdef.get_definition(), bmod.isVirtual());
  }
}


namespace
{
  ClassKeyType classDefinition_opt(SgDeclarationStatement& n)
  {
    SgDeclarationStatement* defdcl = n.get_definingDeclaration();
    SgClassDeclaration*     clsdcl = isSgClassDeclaration(defdcl);

    return defdcl ? SG_DEREF(clsdcl).get_definition()
                  : nullptr
                  ;
  }

  ClassKeyType classDefinition_opt(const SgClassType& n)
  {
    return classDefinition_opt(SG_DEREF( n.get_declaration() ));
  }

  struct ClassDefinition : sg::DispatchHandler<std::pair<ClassKeyType, TypeKeyType> >
  {
      void descend(const SgNode* n)               { res = sg::dispatch(*this, n); }

      void handle(const SgNode& n)                { SG_UNEXPECTED_NODE(n); }
      void handle(const SgType& n)                { res = std::make_pair(nullptr, &n); }

      void handle(const SgModifierType& n)        { descend(n.get_base_type()); }
      void handle(const SgTypedefType& n)         { descend(n.get_base_type()); }
      void handle(const SgPointerType& n)         { descend(n.get_base_type()); }
      void handle(const SgReferenceType& n)       { descend(n.get_base_type()); }
      void handle(const SgRvalueReferenceType& n) { descend(n.get_base_type()); }
      void handle(const SgDeclType& n)            { descend(SG_DEREF(n.get_base_expression()).get_type()); }
      //~ void handle(SgAutoType& n)            { /* what is the real type? */ }

      void handle(const SgClassType& n)           { res = std::make_pair(classDefinition_opt(n), &n); }

      static
      ReturnType get(TypeKeyType n);
  };
}


std::pair<ClassKeyType, TypeKeyType>
getClassCastInfo (TypeKeyType n)
{
  return sg::dispatch(ClassDefinition{}, n);
}

std::ostream& operator<<(std::ostream& os, const CastWriterDbg& cw)
{
  const SgCastExp&    castnode = SG_DEREF(cw.node);
  const Sg_File_Info& where    = SG_DEREF(castnode.get_startOfConstruct());

  os << castnode.unparseToString()
     << " [" << castnode.get_cast_type() << "] "
     << "@" << where.get_line() << ":" << where.get_col();

  return os;
}

int
RoseCompatibilityBridge::compareNames(FunctionId lhs, FunctionId rhs) const
{
  const std::string lhsname{funMap.getFunctionNameFromFunctionId(lhs)};
  const std::string rhsname{funMap.getFunctionNameFromFunctionId(rhs)};
  const bool        lhsIsDtor = lhsname.front() == '~';
  const bool        rhsIsDtor = rhsname.front() == '~';
  int               cmpres = 0;

  firstDecisiveComparison
  || (cmpres = cmpValue(lhsIsDtor, rhsIsDtor))
  || (cmpres = lhsname.compare(rhsname))
  ;

  return cmpres;
}

int compareTypes(const SgType& lhs, const SgType& rhs)
{
  return TypeComparator::compare(lhs, rhs, false, false);
}

int
RoseCompatibilityBridge::compareTypes(FunctionId lhs, FunctionId rhs, bool exclReturn) const
{
  static constexpr bool withArrayDecay = true;

  const SgFunctionType& lhsty = SG_DEREF(funMap.getTypeFromFunctionId(lhs));
  const SgFunctionType& rhsty = SG_DEREF(funMap.getTypeFromFunctionId(rhs));

  return sg::dispatch(TypeComparator{rhsty, withArrayDecay, exclReturn}, &lhsty);
}

VariableId
RoseCompatibilityBridge::variableId(SgInitializedName* var) const
{
  return varMap.variableId(var);
}

VariableId
RoseCompatibilityBridge::variableId(SgVariableDeclaration* var) const
{
  return varMap.variableId(var);
}

std::string
RoseCompatibilityBridge::nameOf(VariableId vid) const
{
  return vid.toString(&varMap);
}

std::string
RoseCompatibilityBridge::nameOf(FunctionId fid) const
{
  return funMap.getFunctionNameFromFunctionId(fid);
}

namespace
{
  struct CovarianceChecker : sg::DispatchHandler<RoseCompatibilityBridge::ReturnTypeRelation>
  {
      using base = sg::DispatchHandler<RoseCompatibilityBridge::ReturnTypeRelation>;

      explicit
      CovarianceChecker(const ClassAnalysis& classAnalysis, const SgType& baseType)
      : base(RoseCompatibilityBridge::unrelated), classes(classAnalysis), baseTy(&baseType)
      {}

      // non class types are checked for type equality
      ReturnType
      areSameOrCovariant(const SgType& drvTy, const SgType& basTy)
      {
        return compareTypes(drvTy, basTy) == 0 ? RoseCompatibilityBridge::sametype
                                               : RoseCompatibilityBridge::unrelated
                                               ;
      }

      ReturnType
      areSameOrCovariant(const SgModifierType& drvTy, const SgModifierType& basTy)
      {
        const SgType& baseBaseTy = SG_DEREF(basTy.get_base_type());

        if (cmpModifier(drvTy.get_typeModifier(), basTy.get_typeModifier()) != 0)
          return RoseCompatibilityBridge::unrelated;

        return sg::dispatch(CovarianceChecker{classes, baseBaseTy}, drvTy.get_base_type());
      }

      // class types are checked for covariance
      ReturnType
      areSameOrCovariant(const SgClassType& drvClass, const SgClassType& basClass)
      {
        if (compareTypes(drvClass, basClass) == 0) return RoseCompatibilityBridge::sametype;

        const SgClassDeclaration* drvDcl0 = isSgClassDeclaration(drvClass.get_declaration());
        const SgClassDeclaration* basDcl0 = isSgClassDeclaration(basClass.get_declaration());
        if (!drvDcl0 || !basDcl0) return RoseCompatibilityBridge::unrelated;

        const SgClassDeclaration* drvDcl  = isSgClassDeclaration(drvDcl0->get_definingDeclaration());
        const SgClassDeclaration* basDcl  = isSgClassDeclaration(basDcl0->get_definingDeclaration());
        if (!drvDcl || !basDcl) return RoseCompatibilityBridge::unrelated;

        const SgClassDefinition*  drvDef  = drvDcl->get_definition();
        const SgClassDefinition*  basDef  = basDcl->get_definition();

        if (drvDef == basDef)
          return RoseCompatibilityBridge::sametype;

        if (classes.areBaseDerived(basDef, drvDef))
          return RoseCompatibilityBridge::covariant;

        return RoseCompatibilityBridge::unrelated;
      }

      ReturnType
      descend(const SgType* ty) const
      {
        return sg::dispatch(*this, ty);
      }

      /// generic template routine to check for covariance
      /// if @chw is null, the check tests for strict equality
      template <class SageType>
      ReturnType
      check(const SageType& drvTy)
      {
        baseTy = skipAliasTypes(baseTy);

        if (drvTy.variantT() != SG_DEREF(baseTy).variantT())
          return RoseCompatibilityBridge::unrelated;

        return areSameOrCovariant(drvTy, static_cast<const SageType&>(*baseTy));
      }

      void handle(const SgNode& drv)                { SG_UNEXPECTED_NODE(drv); }

      void handle(const SgType& drv)                { res = check(drv); }

      // skip typedefs
      void handle(const SgTypedefType& drv)         { res = descend(drv.get_base_type()); }

      // covariance is only maintained through classes and modifiers
      // @{
      void handle(const SgClassType& drv)           { res = check(drv); }
      void handle(const SgModifierType& drv)        { res = check(drv); }
      // @}

      // should have been removed by CovariantPrefixChecker
      // @{
      void handle(const SgReferenceType& drv)       { SG_UNEXPECTED_NODE(drv); }
      void handle(const SgRvalueReferenceType& drv) { SG_UNEXPECTED_NODE(drv); }
      // @}

    private:
      const ClassAnalysis& classes;
      const SgType*        baseTy;
  };


  /// checks that both types have covariant prefixes (e.g., reference, pointer, rvalue-reference)
  ///   to a class type.
  struct CovariantPrefixChecker : sg::DispatchHandler< std::tuple<const SgType*, const SgType*> >
  {
      explicit
      CovariantPrefixChecker(const SgType& baseType)
      : basTy(&baseType)
      {}

      static
      bool typeChk(const SgType&, const SgType&)
      {
        return true;
      }

      static
      bool typeChk(const SgModifierType& derived, const SgModifierType& base)
      {
        return cmpModifier(derived.get_typeModifier(), base.get_typeModifier()) == 0;
      }

      ReturnType
      descend(SgType* ty) const
      {
        return sg::dispatch(*this, ty);
      }

      /// generic template routine to check for covariance
      /// if @chw is null, the check tests for strict equality
      template <class T>
      ReturnType
      check(const T& drvTy)
      {
        basTy = skipAliasTypes(basTy);

        // symmetric check for strict type equality
        if (drvTy.variantT() != SG_DEREF(basTy).variantT())
          return ReturnType{ nullptr, nullptr };

        //~ const T&   baseType    = SG_ASSERT_TYPE(const T, *basTy);
        const T&   baseType    = static_cast<const T&>(*basTy);
        const bool prefixMatch = typeChk(drvTy, baseType);

        return prefixMatch ? ReturnType{ drvTy.get_base_type(), baseType.get_base_type() }
                           : ReturnType{ nullptr, nullptr } ;
      }

      void handle(const SgNode& drv)                { SG_UNEXPECTED_NODE(drv); }

      void handle(const SgType& drv)                {}

      // skip typedefs
      void handle(const SgTypedefType& drv)         { res = descend(drv.get_base_type()); }

      // the modifiers must equal
      void handle(const SgModifierType& drv)        { res = check(drv); }

      // polymorphic root types (must also equal)
      // @{
      void handle(const SgReferenceType& drv)       { res = check(drv); }
      void handle(const SgRvalueReferenceType& drv) { res = check(drv); }
      void handle(const SgPointerType& drv)         { res = check(drv); }
      // @}
    private:
      const SgType* basTy;
  };
}

RoseCompatibilityBridge::ReturnTypeRelation
RoseCompatibilityBridge::haveSameOrCovariantReturn(const ClassAnalysis& classes, FunctionId basId, FunctionId drvId) const
{
  using PolymorphicRootTypes = CovariantPrefixChecker::ReturnType;

  const SgFunctionType&      basTy = SG_DEREF(funMap.getTypeFromFunctionId(basId));
  const SgFunctionType&      drvTy = SG_DEREF(funMap.getTypeFromFunctionId(drvId));
  const SgType&              basRet = SG_DEREF(basTy.get_return_type());
  const SgType&              drvRet = SG_DEREF(drvTy.get_return_type());

  // skip polymorphic root (e.g., reference, pointer)
  const PolymorphicRootTypes rootTypes = sg::dispatch(CovariantPrefixChecker{basRet}, &drvRet);
  const SgType*              drvRoot = std::get<0>(rootTypes);

  if (!drvRoot)
  {
    //~ std::cerr << "w/o root: " << basId.getIdCode() << "<>" << drvId.getIdCode() << std::endl;
    return sg::dispatch(CovarianceChecker{classes, basRet}, &drvRet);
  }

  //~ std::cerr << "w/  root: " << basId.getIdCode() << "<>" << drvId.getIdCode() << std::endl;
  // test if the roots are covariant
  return sg::dispatch(CovarianceChecker{classes, SG_DEREF(std::get<1>(rootTypes))}, drvRoot);
}


FunctionId
RoseCompatibilityBridge::functionId(const SgFunctionDeclaration* fun) const
{
  return funMap.getFunctionIdFromDeclaration(fun);
}

void
RoseCompatibilityBridge::extractFromProject(ClassAnalysis& classes, CastAnalysis& casts, ASTRootType n) const
{
  NodeCollector::NodeTracker visited;

  sg::dispatch(NodeCollector{visited, classes, casts, *this}, n);
}

bool
RoseCompatibilityBridge::isPureVirtual(FunctionId id) const
{
  return SG_DEREF(funMap.getFunctionDeclaration(id)).get_functionModifier().isPureVirtual();
}

FuncNameFn
RoseCompatibilityBridge::functionNomenclator() const
{
  const RoseCompatibilityBridge* rcb = this;

  return [=](FunctionId id) -> std::string { return rcb->nameOf(id); };
}

ClassNameFn
RoseCompatibilityBridge::classNomenclator() const
{
  return typeNameOf;
}

}

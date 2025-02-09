
#include "sage3basic.h"

#include <limits>

#include "AdaType.h"

#include "sageGeneric.h"
#include "Ada_to_ROSE.h"
#include "AdaExpression.h"
#include "AdaStatement.h"
#include "AdaMaker.h"


// turn on all GCC warnings after include files have been processed
#pragma GCC diagnostic warning "-Wall"
#pragma GCC diagnostic warning "-Wextra"


namespace sb = SageBuilder;


namespace Ada_ROSE_Translation
{

namespace
{
  struct MakeTyperef : sg::DispatchHandler<SgType*>
  {
      typedef sg::DispatchHandler<SgType*> base;

      MakeTyperef(Element_Struct& elem, AstContext astctx)
      : base(), el(elem), ctx(astctx)
      {}

      void set(SgType* ty)                       { ADA_ASSERT(ty); res = ty; }

      void handle(SgNode& n)                     { SG_UNEXPECTED_NODE(n); }

      void handle(SgAdaFormalTypeDecl& n)        { set(n.get_formal_type()); }
      void handle(SgType& n)                     { set(&n); }
      void handle(SgClassDeclaration& n)         { set(&mkRecordType(n)); }
      void handle(SgAdaTaskTypeDecl& n)          { set(&mkAdaTaskType(n)); }
      void handle(SgEnumDeclaration& n)          { set(n.get_type()); }
      void handle(SgTypedefDeclaration& n)       { set(n.get_type()); }

      void handle(SgAdaAttributeExp& n)
      {
        attachSourceLocation(n, el, ctx);
        set(&mkAttributeType(n));
      }

    private:
      Element_Struct& el;
      AstContext      ctx;
  };

  SgNode&
  getExprTypeID(Element_ID tyid, AstContext ctx);

  SgNode&
  getExprType(Expression_Struct& typeEx, AstContext ctx)
  {
    SgNode* res = nullptr;

    switch (typeEx.Expression_Kind)
    {
      case An_Identifier:
        {
          logKind("An_Identifier");

          // is it a type?
          // typeEx.Corresponding_Name_Declaration ?
          res = findFirst(asisTypes(), typeEx.Corresponding_Name_Definition);

          if (!res)
          {
            // is it an exception?
            // typeEx.Corresponding_Name_Declaration ?
            res = findFirst(asisExcps(), typeEx.Corresponding_Name_Definition);
          }

          if (!res)
          {
            // is it a predefined Ada type?
            res = findFirst(adaTypes(), AdaIdentifier{typeEx.Name_Image});
          }

          if (!res)
          {
            // what is it?
            logWarn() << "unknown type name: " << typeEx.Name_Image
                      << " / " << typeEx.Corresponding_Name_Definition
                      << std::endl;

            ADA_ASSERT(!FAIL_ON_ERROR(ctx));
            res = sb::buildVoidType();
          }

          break /* counted in getExpr */;
        }

      case A_Selected_Component:
        {
          logKind("A_Selected_Component");
          res = &getExprTypeID(typeEx.Selector, ctx);
          break /* counted in getExpr */;
        }

      case An_Attribute_Reference:
        {
          logKind("An_Attribute_Reference");
          res = &getAttributeExpr(typeEx, ctx);
          break ;
        }

      default:
        logWarn() << "Unknown type expression: " << typeEx.Expression_Kind << std::endl;
        ADA_ASSERT(!FAIL_ON_ERROR(ctx));
        res = sb::buildVoidType();
    }

    return SG_DEREF(res);
  }

  SgNode&
  getExprTypeID(Element_ID tyid, AstContext ctx)
  {
    Element_Struct& elem = retrieveAs<Element_Struct>(elemMap(), tyid);
    ADA_ASSERT(elem.Element_Kind == An_Expression);

    return getExprType(elem.The_Union.Expression, ctx);
  }


  SgType&
  getAccessType(Definition_Struct& def, AstContext ctx)
  {
    ADA_ASSERT(def.Definition_Kind == An_Access_Definition);

    logKind("An_Access_Definition");

    SgType* res = nullptr;
    Access_Definition_Struct& access = def.The_Union.The_Access_Definition;

    switch (access.Access_Definition_Kind)
    {
      case An_Anonymous_Access_To_Constant:            // [...] access constant subtype_mark
      case An_Anonymous_Access_To_Variable:            // [...] access subtype_mark
        {
          const bool isConstant = access.Access_Definition_Kind == An_Anonymous_Access_To_Constant;
          logKind(isConstant ? "An_Anonymous_Access_To_Constant" : "An_Anonymous_Access_To_Variable");

          SgType& ty = getDeclTypeID(access.Anonymous_Access_To_Object_Subtype_Mark, ctx);

          res = &mkAdaAccessType(&ty);

          /** unused fields:
                 bool                         Has_Null_Exclusion;
           */
          break;
        }

      case An_Anonymous_Access_To_Procedure:           // access procedure
      case An_Anonymous_Access_To_Protected_Procedure: // access protected procedure
      case An_Anonymous_Access_To_Function:            // access function
      case An_Anonymous_Access_To_Protected_Function:  // access protected function
      case Not_An_Access_Definition: /* break; */ // An unexpected element
      default:
        logWarn() << "ak? " << access.Access_Definition_Kind << std::endl;
        res = &mkAdaAccessType(sb::buildVoidType());
        ADA_ASSERT(!FAIL_ON_ERROR(ctx));
    }

    return SG_DEREF(res);
  }

  SgType&
  getDeclType(Element_Struct& elem, AstContext ctx)
  {
    if (elem.Element_Kind == An_Expression)
    {
      SgNode& basenode = getExprType(elem.The_Union.Expression, ctx);
      SgType* res      = sg::dispatch(MakeTyperef(elem, ctx), &basenode);

      return SG_DEREF(res);
    }

    ADA_ASSERT(elem.Element_Kind == A_Definition);
    Definition_Struct& def = elem.The_Union.Definition;

    if (def.Definition_Kind == An_Access_Definition)
      return getAccessType(def, ctx);

    logError() << "getDeclType: unhandled definition kind: " << def.Definition_Kind
               << std::endl;
    ADA_ASSERT(!FAIL_ON_ERROR(ctx));
    return SG_DEREF(sb::buildVoidType());
  }


  SgAdaTypeConstraint*
  getConstraintID_opt(Element_ID el, AstContext ctx)
  {
    return el ?  &getConstraintID(el, ctx) : nullptr;
  }


  SgClassDefinition&
  getRecordBody(Record_Definition_Struct& rec, AstContext ctx)
  {
    SgClassDefinition&        sgnode     = mkRecordBody();
    ElemIdRange               components = idRange(rec.Record_Components);
    //~ ElemIdRange               implicits  = idRange(rec.Implicit_Components);

    traverseIDs(components, elemMap(), ElemCreator{ctx.scope_npc(sgnode)});

    // how to represent implicit components
    //~ traverseIDs(implicits, elemMap(), ElemCreator{ctx.scope_npc(sgnode)});

    /* unused nodes:
         Record_Component_List Implicit_Components
    */

    markCompilerGenerated(sgnode);
    return sgnode;
  }

  SgClassDefinition&
  getRecordBodyID(Element_ID recid, AstContext ctx)
  {
    Element_Struct&           elem = retrieveAs<Element_Struct>(elemMap(), recid);
    ADA_ASSERT(elem.Element_Kind == A_Definition);

    Definition_Struct&        def = elem.The_Union.Definition;

    if (def.Definition_Kind == A_Null_Record_Definition)
    {
      logKind("A_Null_Record_Definition");
      SgClassDefinition&      sgdef = mkRecordBody();

      attachSourceLocation(sgdef, elem, ctx);
      return sgdef;
    }

    ADA_ASSERT(def.Definition_Kind == A_Record_Definition);

    logKind("A_Record_Definition");
    return getRecordBody(def.The_Union.The_Record_Definition, ctx);
  }

  SgClassDeclaration&
  getParentRecordDecl(Definition_Struct& def, AstContext ctx)
  {
    ADA_ASSERT(def.Definition_Kind == A_Subtype_Indication);

    logKind("A_Subtype_Indication");

    Subtype_Indication_Struct& subtype = def.The_Union.The_Subtype_Indication;
    ADA_ASSERT (subtype.Subtype_Constraint == 0);

    Element_Struct&            subelem = retrieveAs<Element_Struct>(elemMap(), subtype.Subtype_Mark);
    ADA_ASSERT(subelem.Element_Kind == An_Expression);

    SgNode*                    basenode = &getExprType(subelem.The_Union.Expression, ctx);
    SgClassDeclaration*        res = isSgClassDeclaration(basenode);

    if (res == nullptr)
    {
      logError() << "getParentRecordDecl: " << typeid(*basenode).name() << std::endl;
      ROSE_ABORT();
    }

    return SG_DEREF(res);
  }


  struct EnumElementCreator
  {
      EnumElementCreator(SgEnumDeclaration& n, AstContext astctx)
      : enumdcl(n), enumty(SG_DEREF(n.get_type())), ctx(astctx)
      {}

      void operator()(Element_Struct& elem)
      {
        ADA_ASSERT(elem.Element_Kind == A_Declaration);
        logKind("A_Declaration");

        Declaration_Struct& decl = elem.The_Union.Declaration;
        ADA_ASSERT(decl.Declaration_Kind == An_Enumeration_Literal_Specification);
        logKind("An_Enumeration_Literal_Specification");

        NameData            name = singleName(decl, ctx);
        ADA_ASSERT(name.ident == name.fullName);

        // \todo name.ident could be a character literal, such as 'c'
        //       since SgEnumDeclaration only accepts SgInitializedName as enumerators
        //       SgInitializedName are created with the name 'c' instead of character constants.
        SgInitializedName&  sgnode = mkInitializedName(name.ident, enumty, nullptr);

        sgnode.set_scope(enumdcl.get_scope());
        attachSourceLocation(sgnode, elem, ctx);
        //~ sg::linkParentChild(enumdcl, sgnode, &SgEnumDeclaration::append_enumerator);
        enumdcl.append_enumerator(&sgnode);
        ADA_ASSERT(sgnode.get_parent() == &enumdcl);

        recordNode(asisVars(), name.id(), sgnode);
      }

    private:
      SgEnumDeclaration& enumdcl;
      SgType&            enumty;
      AstContext         ctx;
  };

  TypeData
  getFormalTypeFoundation(const std::string& name, Definition_Struct& def, AstContext ctx)
  {
    ADA_ASSERT(def.Definition_Kind == A_Formal_Type_Definition);
    logKind("A_Formal_Type_Definition");

    Formal_Type_Definition_Struct& typenode = def.The_Union.The_Formal_Type_Definition;
    TypeData                       res{nullptr, false, false, false};

    switch (typenode.Formal_Type_Kind)
      {
        // MS: types relevant right now

      case A_Formal_Private_Type_Definition:         // 12.5.1(2)   -> Trait_Kinds
        {
          logKind("A_Formal_Private_Type_Definition");
          SgAdaFormalType* t = &mkAdaFormalType(name);
          if (typenode.Has_Private) {
            t->set_is_private(true);
          }
          if (typenode.Has_Limited) {
            t->set_is_limited(true);
          }
          res.n = t;
          break;
        }

      case A_Formal_Access_Type_Definition:          // 3.10(3),3.10(5)
        // TODO: finish me
        std::cout << "*********** getFormalTypeFoundation Formal_Access_type_Definition" << std::endl;
        ADA_ASSERT(false);
        logKind("A_Formal_Access_Type_Definition");
        break;

        // MS: types to do later when we need them
      case A_Formal_Tagged_Private_Type_Definition:  // 12.5.1(2)   -> Trait_Kinds
      case A_Formal_Derived_Type_Definition:         // 12.5.1(3)   -> Trait_Kinds
      case A_Formal_Discrete_Type_Definition:        // 12.5.2(2)
      case A_Formal_Signed_Integer_Type_Definition:  // 12.5.2(3)
      case A_Formal_Modular_Type_Definition:         // 12.5.2(4)
      case A_Formal_Floating_Point_Definition:       // 12.5.2(5)
      case A_Formal_Ordinary_Fixed_Point_Definition: // 12.5.2(6)
      case A_Formal_Decimal_Fixed_Point_Definition:  // 12.5.2(7)

        //|A2005 start
      case A_Formal_Interface_Type_Definition:       // 12.5.5(2) -> Interface_Kinds
        //|A2005 end

      case A_Formal_Unconstrained_Array_Definition:  // 3.6(3)
      case A_Formal_Constrained_Array_Definition:    // 3.6(5)

      default:
        logWarn() << "unhandled formal type kind " << typenode.Formal_Type_Kind << std::endl;
        // NOTE: temporarily create an AdaFormalType with the given name,
        //       but set no fields.  This is sufficient to pass some test cases but
        //       is not correct.
        SgAdaFormalType* t = &mkAdaFormalType(name);
        res.n = t;
        //ADA_ASSERT(!FAIL_ON_ERROR);
      }

    return res;
  }

  TypeData
  getTypeFoundation(const std::string& name, Definition_Struct& def, AstContext ctx)
  {
    ADA_ASSERT(def.Definition_Kind == A_Type_Definition);

    logKind("A_Type_Definition");

    Type_Definition_Struct& typenode = def.The_Union.The_Type_Definition;
    TypeData                res{nullptr, false, false, false};

    /* unused fields:
       Definition_Struct
         bool                           Has_Null_Exclusion;
    */

    switch (typenode.Type_Kind)
    {
      case A_Derived_Type_Definition:              // 3.4(2)     -> Trait_Kinds
        {
          logKind("A_Derived_Type_Definition");
          /*
             unused fields: (derivedTypeDef)
                Declaration_List     Implicit_Inherited_Declarations;
          */
          SgType& basetype = getDefinitionTypeID(typenode.Parent_Subtype_Indication, ctx);

          res.n = &mkAdaDerivedType(basetype);
          break;
        }

      case A_Derived_Record_Extension_Definition:  // 3.4(2)     -> Trait_Kinds
        {
          logKind("A_Derived_Record_Extension_Definition");

          SgClassDefinition&  def    = getRecordBodyID(typenode.Record_Definition, ctx);
          SgClassDeclaration& basecl = getParentRecordDeclID(typenode.Parent_Subtype_Indication, ctx);
          SgBaseClass&        parent = mkRecordParent(basecl);

          sg::linkParentChild(def, parent, &SgClassDefinition::append_inheritance);

          /*
          Declaration_List     Implicit_Inherited_Declarations;
          Declaration_List     Implicit_Inherited_Subprograms;
          Declaration          Corresponding_Parent_Subtype;
          Declaration          Corresponding_Root_Type;
          Declaration          Corresponding_Type_Structure;
          Expression_List      Definition_Interface_List;
          */
          res.n = &def;
          break;
        }

      case An_Enumeration_Type_Definition:         // 3.5.1(2)
        {
          ADA_ASSERT(name.size());

          logKind("An_Enumeration_Type_Definition");

          SgEnumDeclaration& sgnode = mkEnumDecl(name, ctx.scope());
          ElemIdRange        enums = idRange(typenode.Enumeration_Literal_Declarations);

          traverseIDs(enums, elemMap(), EnumElementCreator{sgnode, ctx});
          /* unused fields:
           */
          res.n = &sgnode;
          break ;
        }

      case A_Signed_Integer_Type_Definition:       // 3.5.4(3)
        {
          logKind("A_Signed_Integer_Type_Definition");

          SgAdaTypeConstraint& constraint = getConstraintID(typenode.Integer_Constraint, ctx);
          SgTypeInt&           superty    = SG_DEREF(sb::buildIntType());

          res.n = &mkAdaSubtype(superty, constraint, true);
          /* unused fields:
           */
          break;
        }

      case A_Modular_Type_Definition:              // 3.5.4(4)
        {
          logKind("A_Modular_Type_Definition");

          SgExpression& modexpr = getExprID(typenode.Mod_Static_Expression, ctx);

          res.n = &mkAdaModularType(modexpr);
          /* unused fields:
           */
          break;
        }

      case A_Floating_Point_Definition:            // 3.5.7(2)
        {
          logKind("A_Floating_Point_Definition");

          SgExpression&         digits     = getExprID_opt(typenode.Digits_Expression, ctx);
          SgAdaTypeConstraint*  constraint = getConstraintID_opt(typenode.Real_Range_Constraint, ctx);
          SgAdaRangeConstraint* rngconstr  = isSgAdaRangeConstraint(constraint);
          ADA_ASSERT(!constraint || rngconstr);

          res.n = &mkAdaFloatType(digits, rngconstr);
          break;
        }

      case A_Constrained_Array_Definition:         // 3.6(2)
        {
          logKind("A_Constrained_Array_Definition");

          ElemIdRange                indicesAsis = idRange(typenode.Discrete_Subtype_Definitions);
          std::vector<SgExpression*> indicesSeq  = traverseIDs(indicesAsis, elemMap(), ExprSeqCreator{ctx});
          SgExprListExp&             indicesAst  = mkExprListExp(indicesSeq);
          SgType&                    compType    = getDefinitionTypeID(typenode.Array_Component_Definition, ctx);

          res.n = &mkArrayType(compType, indicesAst, false /* constrained */);
          ADA_ASSERT(indicesAst.get_parent());
          /* unused fields:
          */
          break ;
        }

      case An_Unconstrained_Array_Definition:      // 3.6(2)
        {
          logKind("An_Unconstrained_Array_Definition");

          ElemIdRange                indicesAsis = idRange(typenode.Index_Subtype_Definitions);
          std::vector<SgExpression*> indicesSeq  = traverseIDs(indicesAsis, elemMap(), ExprSeqCreator{ctx});
          SgExprListExp&             indicesAst  = mkExprListExp(indicesSeq);
          SgType&                    compType    = getDefinitionTypeID(typenode.Array_Component_Definition, ctx);

          res.n = &mkArrayType(compType, indicesAst, true /* unconstrained */);
          ADA_ASSERT(indicesAst.get_parent());
          /* unused fields:
          */
          break;
        }

      case A_Record_Type_Definition:               // 3.8(2)     -> Trait_Kinds
      case A_Tagged_Record_Type_Definition:        // 3.8(2)     -> Trait_Kinds
        {
          logKind(typenode.Type_Kind == A_Record_Type_Definition ? "A_Record_Type_Definition" : "A_Tagged_Record_Type_Definition");

          SgClassDefinition& def = getRecordBodyID(typenode.Record_Definition, ctx);

          (typenode.Has_Tagged ? logWarn() : logTrace())
             << "Type_Definition_Struct::tagged set ? " << typenode.Has_Tagged
             << std::endl;

          res = TypeData{&def, typenode.Has_Abstract, typenode.Has_Limited, typenode.Type_Kind == A_Tagged_Record_Type_Definition};
          /*
             unused fields (A_Record_Type_Definition):

             unused fields (A_Tagged_Record_Type_Definition):
                bool                 Has_Private;
                bool                 Has_Tagged;
                Declaration_List     Corresponding_Type_Operators;

             break;
          */
          break;
        }

      case An_Access_Type_Definition:              // 3.10(2)    -> Access_Type_Kinds
        {
          logKind("An_Access_Type_Definition");
          Access_Type_Struct access_type = typenode.Access_Type;
          auto access_type_kind = access_type.Access_Type_Kind;
          bool isFuncAccess = false;

          switch (access_type_kind) {
          // variable access kinds
          case A_Pool_Specific_Access_To_Variable:
          case An_Access_To_Variable:
          case An_Access_To_Constant:
            {
              SgType& ato = getDefinitionTypeID(access_type.Access_To_Object_Definition, ctx);
              res.n = &mkAdaAccessType(&ato);
              // handle cases for ALL or CONSTANT general access modifiers
              switch (access_type_kind) {
              case An_Access_To_Variable:
                ((SgAdaAccessType*)res.n)->set_is_general_access(true);
                break;
              case An_Access_To_Constant:
                ((SgAdaAccessType*)res.n)->set_is_constant(true);
                break;
              default:
                break;
              }

              break;
            }

          // subprogram access kinds
          case An_Access_To_Function:
          case An_Access_To_Protected_Function:
          case An_Access_To_Procedure:
          case An_Access_To_Protected_Procedure:
            {
              logWarn() << "subprogram access type support incomplete" << std::endl;

              if (access_type_kind == An_Access_To_Function ||
                  access_type_kind == An_Access_To_Protected_Function) {
                // these are functions, so we need to worry about return types
                isFuncAccess = true;
              }

              if (access_type.Access_To_Subprogram_Parameter_Profile.Length > 0) {
                logWarn() << "subprogram access types with parameter profiles not supported." << std::endl;
                /*
                ElemIdRange range = idRange(access_type.Access_To_Subprogram_Parameter_Profile);

                SgFunctionParameterList& lst   = mkFunctionParameterList();
                SgFunctionParameterScope& psc  = mkLocatedNode<SgFunctionParameterScope>(&mkFileInfo());
                ParameterCompletion{range,ctx}(lst, ctx);

                ((SgAdaAccessType*)res.n)->set_subprogram_profile(&lst);
                */
              }

              res.n = &mkAdaAccessType(NULL);
              ((SgAdaAccessType*)res.n)->set_is_object_type(false);

              if (isFuncAccess) {
                SgType &rettype = getDeclTypeID(access_type.Access_To_Function_Result_Profile, ctx);
                ((SgAdaAccessType*)res.n)->set_return_type(&rettype);
              }

              // if protected, set the flag
              if (access_type_kind == An_Access_To_Protected_Procedure ||
                  access_type_kind == An_Access_To_Protected_Function) {
                ((SgAdaAccessType*)res.n)->set_is_protected(true);
              }

              break;
            }
          default:
            logWarn() << "Unhandled access type kind." << std::endl;
            res.n = sb::buildVoidType();
          }

          break;
        }

      case Not_A_Type_Definition: /* break; */     // An unexpected element
      case A_Root_Type_Definition:                 // 3.5.4(14):  3.5.6(3)
      case An_Ordinary_Fixed_Point_Definition:     // 3.5.9(3)
      case A_Decimal_Fixed_Point_Definition:       // 3.5.9(6)
      //  //|A2005 start
      case An_Interface_Type_Definition:           // 3.9.4      -> Interface_Kinds
      //  //|A2005 end
      default:
        {
          logWarn() << "unhandled type kind " << typenode.Type_Kind << std::endl;
          ADA_ASSERT(!FAIL_ON_ERROR(ctx));
          res.n = sb::buildVoidType();
        }
    }

    ADA_ASSERT(res.n);
    return res;
  }

  SgType&
  getDefinitionType(Definition_Struct& def, AstContext ctx)
  {
    SgType* res = nullptr;

    switch (def.Definition_Kind)
    {
      case A_Type_Definition:
        {
          TypeData resdata = getTypeFoundation("", def, ctx);

          res = isSgType(resdata.n);
          ADA_ASSERT(res);
          break;
        }

      case A_Subtype_Indication:
        {
          logKind("A_Subtype_Indication");

          Subtype_Indication_Struct& subtype   = def.The_Union.The_Subtype_Indication;

          res = &getDeclTypeID(subtype.Subtype_Mark, ctx);

          // \todo if there is no subtype constraint, shall we produce
          //       a subtype w/ NoConstraint, or leave the original type?
          if (subtype.Subtype_Constraint)
          {
            SgAdaTypeConstraint& range = getConstraintID(subtype.Subtype_Constraint, ctx);

            res = &mkAdaSubtype(SG_DEREF(res), range);
          }

          /* unused fields:
                bool       Has_Null_Exclusion;
          */
          break;
        }

      case A_Component_Definition:
        {
          logKind("A_Component_Definition");

          Component_Definition_Struct& component = def.The_Union.The_Component_Definition;

#if ADA_2005_OR_MORE_RECENT
          res = &getDefinitionTypeID(component.Component_Definition_View, ctx);
#else
          res = &getDefinitionTypeID(component.Component_Subtype_Indication, ctx);
#endif /* ADA_2005_OR_MORE_RECENT */

          /* unused fields:
               bool       Has_Aliased;
          */
          break;
        }

      default:
        logWarn() << "Unhandled type definition: " << def.Definition_Kind << std::endl;
        res = sb::buildVoidType();
        ADA_ASSERT(!FAIL_ON_ERROR(ctx));
    }

    return SG_DEREF(res);
  }

  SgTypedefDeclaration&
  declareIntSubtype(const std::string& name, int64_t lo, int64_t hi, SgAdaPackageSpec& scope)
  {
    SgTypeInt&            ty = SG_DEREF(sb::buildIntType());
    SgIntVal&             lb = SG_DEREF(sb::buildIntVal(lo));
    SgIntVal&             ub = SG_DEREF(sb::buildIntVal(hi));
    SgRangeExp&           range = mkRangeExp(lb, ub);
    SgAdaRangeConstraint& constraint = mkAdaRangeConstraint(range);
    SgAdaSubtype&         subtype = mkAdaSubtype(ty, constraint);
    SgTypedefDeclaration& sgnode = mkTypeDecl(name, subtype, scope);

    scope.append_statement(&sgnode);
    return sgnode;
  }

  SgTypedefDeclaration&
  declareStringType(const std::string& name, SgType& positive, SgType& comp, SgAdaPackageSpec& scope)
  {
    SgExprListExp&        idx     = mkExprListExp({sb::buildTypeExpression(&positive)});
    SgArrayType&          strtype = mkArrayType(comp, idx, true);
    SgTypedefDeclaration& sgnode  = mkTypeDecl(name, strtype, scope);

    scope.append_statement(&sgnode);
    return sgnode;
  }

  SgInitializedName&
  declareException(const std::string& name, SgType& base, SgAdaPackageSpec& scope)
  {
    SgInitializedName&              sgnode = mkInitializedName(name, base, nullptr);
    std::vector<SgInitializedName*> exdecl{ &sgnode };
    SgVariableDeclaration&          exvar = mkExceptionDecl(exdecl, scope);

    exvar.set_firstNondefiningDeclaration(&exvar);
    scope.append_statement(&exvar);
    return sgnode;
  }

  SgAdaPackageSpecDecl&
  declarePackage(const std::string& name, SgAdaPackageSpec& scope)
  {
    SgAdaPackageSpecDecl& sgnode = mkAdaPackageSpecDecl(name, scope);
    SgAdaPackageSpec&     pkgspec = SG_DEREF(sgnode.get_definition());

    scope.append_statement(&sgnode);
    sgnode.set_scope(&scope);

    markCompilerGenerated(pkgspec);
    markCompilerGenerated(sgnode);
    return sgnode;
  }
} // anonymous

std::pair<SgInitializedName*, SgAdaRenamingDecl*>
getExceptionBase(Element_Struct& el, AstContext ctx)
{
  ADA_ASSERT(el.Element_Kind == An_Expression);

  NameData        name = getQualName(el, ctx);
  Element_Struct& elem = name.elem();

  ADA_ASSERT(elem.Element_Kind == An_Expression);
  Expression_Struct& ex  = elem.The_Union.Expression;

  //~ use this if package standard is included
  //~ return lookupNode(asisExcps(), ex.Corresponding_Name_Definition);

  // first try: look up in user defined exceptions
  if (SgInitializedName* ini = findFirst(asisExcps(), ex.Corresponding_Name_Definition))
    return std::make_pair(ini, nullptr);

  // second try: look up in renamed declarations
  if (SgDeclarationStatement* dcl = findFirst(asisDecls(), ex.Corresponding_Name_Definition))
  {
    SgAdaRenamingDecl& rendcl = SG_DEREF(isSgAdaRenamingDecl(dcl));

    return std::make_pair(nullptr, &rendcl);
  }

  // third try: look up in exceptions defined in standard
  if (SgInitializedName* ini = findFirst(adaExcps(), AdaIdentifier{ex.Name_Image}))
    return std::make_pair(ini, nullptr);

  // last resort: create a new initialized name representing the exception
  ADA_ASSERT(!FAIL_ON_ERROR(ctx));
  logError() << "Unknown exception: " << ex.Name_Image << std::endl;

  // \todo create an SgInitializedName if the exception was not found
  SgInitializedName& init = mkInitializedName(ex.Name_Image, lookupNode(adaTypes(), AdaIdentifier{"Exception"}), nullptr);

  init.set_scope(&ctx.scope());
  return std::make_pair(&init, nullptr);
}


SgAdaTypeConstraint&
getConstraintID(Element_ID el, AstContext ctx)
{
  if (isInvalidId(el))
  {
    logWarn() << "Uninitialized element [range constraint]" << std::endl;
    return mkAdaRangeConstraint(mkRangeExp());
  }

  SgAdaTypeConstraint*  res = nullptr;
  Element_Struct&       elem = retrieveAs<Element_Struct>(elemMap(), el);
  ADA_ASSERT(elem.Element_Kind == A_Definition);

  Definition_Struct&    def = elem.The_Union.Definition;
  ADA_ASSERT(def.Definition_Kind == A_Constraint);

  logKind("A_Constraint");

  Constraint_Struct&    constraint = def.The_Union.The_Constraint;

  switch (constraint.Constraint_Kind)
  {
    case A_Simple_Expression_Range:             // 3.2.2: 3.5(3)
      {
        logKind("A_Simple_Expression_Range");

        SgExpression& lb       = getExprID(constraint.Lower_Bound, ctx);
        SgExpression& ub       = getExprID(constraint.Upper_Bound, ctx);
        SgRangeExp&   rangeExp = mkRangeExp(lb, ub);

        res = &mkAdaRangeConstraint(rangeExp);
        break;
      }

    case A_Range_Attribute_Reference:           // 3.5(2)
      {
        logKind("A_Range_Attribute_Reference");

        SgExpression& rangeExp = getExprID(constraint.Range_Attribute, ctx);

        res = &mkAdaRangeConstraint(rangeExp);
        break;
      }

    case An_Index_Constraint:                   // 3.2.2: 3.6.1
      {
        logKind("An_Index_Constraint");

        ElemIdRange         idxranges = idRange(constraint.Discrete_Ranges);
        SgExpressionPtrList ranges = traverseIDs(idxranges, elemMap(), RangeListCreator{ctx});

        res = &mkAdaIndexConstraint(std::move(ranges));
        break;
      }

    case Not_A_Constraint: /* break; */         // An unexpected element
    case A_Digits_Constraint:                   // 3.2.2: 3.5.9
    case A_Delta_Constraint:                    // 3.2.2: J.3
    case A_Discriminant_Constraint:             // 3.2.2
    default:
      logWarn() << "Unhandled constraint: " << constraint.Constraint_Kind << std::endl;
      ADA_ASSERT(!FAIL_ON_ERROR(ctx));
      res = &mkAdaRangeConstraint(mkRangeExp());
  }

  attachSourceLocation(SG_DEREF(res), elem, ctx);
  return *res;
}


SgType&
getDeclTypeID(Element_ID id, AstContext ctx)
{
  return getDeclType(retrieveAs<Element_Struct>(elemMap(), id), ctx);
}


SgType&
getDefinitionTypeID(Element_ID defid, AstContext ctx)
{
  if (isInvalidId(defid))
  {
    logWarn() << "undefined type id: " << defid << std::endl;
    return SG_DEREF(sb::buildVoidType());
  }

  Element_Struct&     elem = retrieveAs<Element_Struct>(elemMap(), defid);
  ADA_ASSERT(elem.Element_Kind == A_Definition);

  return getDefinitionType(elem.The_Union.Definition, ctx);
}

SgClassDeclaration&
getParentRecordDeclID(Element_ID defid, AstContext ctx)
{
  Element_Struct&     elem = retrieveAs<Element_Struct>(elemMap(), defid);
  ADA_ASSERT(elem.Element_Kind == A_Definition);

  return getParentRecordDecl(elem.The_Union.Definition, ctx);
}

TypeData
getFormalTypeFoundation(const std::string& name, Declaration_Struct& decl, AstContext ctx)
{
  ADA_ASSERT( decl.Declaration_Kind == A_Formal_Type_Declaration );
  Element_Struct&         elem = retrieveAs<Element_Struct>(elemMap(), decl.Type_Declaration_View);
  ADA_ASSERT(elem.Element_Kind == A_Definition);
  Definition_Struct&      def = elem.The_Union.Definition;
  ADA_ASSERT(def.Definition_Kind == A_Formal_Type_Definition);
  return getFormalTypeFoundation(name, def, ctx);
}

TypeData
getTypeFoundation(const std::string& name, Declaration_Struct& decl, AstContext ctx)
{
  ADA_ASSERT( decl.Declaration_Kind == An_Ordinary_Type_Declaration );

  Element_Struct&         elem = retrieveAs<Element_Struct>(elemMap(), decl.Type_Declaration_View);
  ADA_ASSERT(elem.Element_Kind == A_Definition);

  Definition_Struct&      def = elem.The_Union.Definition;
  ADA_ASSERT(def.Definition_Kind == A_Type_Definition);

  return getTypeFoundation(name, def, ctx);
}

void initializePkgStandard(SgGlobal& global)
{
  // make available declarations from the package standard
  // https://www.adaic.org/resources/add_content/standards/05rm/html/RM-A-1.html

  constexpr auto ADAMAXINT = std::numeric_limits<int>::max();

  SgAdaPackageSpecDecl& stddecl = mkAdaPackageSpecDecl("Standard", global);
  SgAdaPackageSpec&     stdspec = SG_DEREF(stddecl.get_definition());

  stddecl.set_scope(&global);

  // \todo reconsider using a true Ada exception representation
  SgType&               exceptionType = SG_DEREF(sb::buildOpaqueType("Exception", &stdspec));

  adaTypes()["EXCEPTION"]           = &exceptionType;

  // \todo reconsider modeling Boolean as an enumeration of True and False
  adaTypes()["BOOLEAN"]             = sb::buildBoolType();

  // integral types
  SgType& intType                   = SG_DEREF(sb::buildIntType());
  SgType& characterType             = SG_DEREF(sb::buildCharType());
  SgType& wideCharacterType         = SG_DEREF(sb::buildChar16Type());
  SgType& wideWideCharacterType     = SG_DEREF(sb::buildChar32Type());

  adaTypes()["INTEGER"]             = &intType;
  adaTypes()["CHARACTER"]           = &characterType;
  adaTypes()["WIDE_CHARACTER"]      = &wideCharacterType;
  adaTypes()["WIDE_WIDE_CHARACTER"] = &wideWideCharacterType;
  adaTypes()["LONG_INTEGER"]        = sb::buildLongType(); // Long int
  adaTypes()["LONG_LONG_INTEGER"]   = sb::buildLongLongType(); // Long long int
  adaTypes()["SHORT_INTEGER"]       = sb::buildShortType(); // Long long int
  adaTypes()["SHORT_SHORT_INTEGER"] = declareIntSubtype("Short_Short_Integer", -(1 << 7), (1 << 7)-1, stdspec).get_type();

  // \todo floating point types
  adaTypes()["FLOAT"]               = sb::buildFloatType();  // Float is a subtype of Real
  adaTypes()["SHORT_FLOAT"]         = sb::buildFloatType();  // Float is a subtype of Real
  adaTypes()["LONG_FLOAT"]          = sb::buildDoubleType(); // Float is a subtype of Real
  adaTypes()["LONG_LONG_FLOAT"]     = sb::buildLongDoubleType(); // Long long Double?

  // int subtypes
  SgType& positiveType              = SG_DEREF(declareIntSubtype("Positive", 1, ADAMAXINT, stdspec).get_type());

  adaTypes()["POSITIVE"]            = &positiveType;
  adaTypes()["NATURAL"]             = declareIntSubtype("Natural",  0, ADAMAXINT, stdspec).get_type();


  // String types
  adaTypes()["STRING"]              = declareStringType("String",           positiveType, characterType,         stdspec).get_type();
  adaTypes()["WIDE_STRING"]         = declareStringType("Wide_String",      positiveType, wideCharacterType,     stdspec).get_type();
  adaTypes()["WIDE_WIDE_STRING"]    = declareStringType("Wide_Wide_String", positiveType, wideWideCharacterType, stdspec).get_type();

  // Ada standard exceptions
  adaExcps()["CONSTRAINT_ERROR"]    = &declareException("Constraint_Error", exceptionType, stdspec);
  adaExcps()["PROGRAM_ERROR"]       = &declareException("Program_Error",    exceptionType, stdspec);
  adaExcps()["STORAGE_ERROR"]       = &declareException("Storage_Error",    exceptionType, stdspec);
  adaExcps()["TASKING_ERROR"]       = &declareException("Tasking_Error",    exceptionType, stdspec);

  // added packages
  adaPkgs()["STANDARD.ASCII"]       = &declarePackage("Ascii", stdspec);
  adaPkgs()["ASCII"]                = adaPkgs()["STANDARD.ASCII"];
}



void ExHandlerTypeCreator::operator()(Element_Struct& elem)
{
  SgExpression* exceptExpr = nullptr;

  if (elem.Element_Kind == An_Expression)
  {
    auto              expair = getExceptionBase(elem, ctx);
    SgScopeStatement& scope = ctx.scope();

    exceptExpr = expair.first ? &mkExceptionRef(*expair.first, scope)
                              : &mkAdaRenamingRefExp(SG_DEREF(expair.second))
                              ;

    attachSourceLocation(SG_DEREF(exceptExpr), elem, ctx);
  }
  else if (elem.Element_Kind == A_Definition)
  {
    exceptExpr = &getDefinitionExpr(elem, ctx);
  }

  lst.push_back(&mkExceptionType(SG_DEREF(exceptExpr)));
}

ExHandlerTypeCreator::operator SgType&() const
{
  ADA_ASSERT(lst.size() > 0);

  if (lst.size() == 1)
    return SG_DEREF(lst[0]);

  return mkTypeUnion(lst);
}

}

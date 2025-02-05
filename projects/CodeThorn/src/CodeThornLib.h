#ifndef CODETHORN_LIB_H
#define CODETHORN_LIB_H

#include "Rose/Diagnostics.h"
#include "Normalization.h"
#include "IOAnalyzer.h"
#include "TimingCollector.h"
#include "AbstractValue.h"

namespace CodeThorn {
  extern Sawyer::Message::Facility logger;
  void initDiagnostics();

  // deprecated, use CodeThornLib::evaluateExpressionWithEmptyState
  AbstractValue evaluateExpressionWithEmptyState(SgExpression* expr);

  namespace CodeThornLib {
    void turnOffRoseWarnings();
    void configureRose();
    AbstractValue evaluateExpressionWithEmptyState(SgExpression* expr);
    void optionallyRunExprEvalTestAndExit(CodeThornOptions& ctOpt,int argc, char * argv[]);
    void optionallyInitializePatternSearchSolver(CodeThornOptions& ctOpt,IOAnalyzer* analyzer,TimingCollector& timingCollector);
    void optionallySetRersMapping(CodeThornOptions ctOpt,LTLOptions ltlOptions,IOAnalyzer* analyzer);

    /* reads and parses LTL rers mapping file into the alphabet
       sets. Returns false if reading fails. Exits with error message if
       format is wrong. */
    bool readAndParseLTLRersMappingFile(string ltlRersMappingFileName, LtlRersMapping& ltlRersMapping);
    void processCtOptGenerateAssertions(CodeThornOptions& ctOpt, CTAnalysis* analyzer, SgProject* root);
    void optionallyRunInternalChecks(CodeThornOptions& ctOpt, int argc, char * argv[]);
    void optionallyRunInliner(CodeThornOptions& ctOpt, Normalization& normalization, SgProject* sageProject);
    void optionallyRunVisualizer(CodeThornOptions& ctOpt, CTAnalysis* analyzer, SgNode* root);
    void optionallyGenerateExternalFunctionsFile(CodeThornOptions& ctOpt, FunctionCallMapping* funCallMapping);
    void optionallyGenerateAstStatistics(CodeThornOptions& ctOpt, SgProject* sageProject);
    void optionallyGenerateSourceProgramAndExit(CodeThornOptions& ctOpt, SgProject* sageProject);
    void optionallyGenerateTraversalInfoAndExit(CodeThornOptions& ctOpt, SgProject* sageProject);
    void optionallyRunRoseAstChecksAndExit(CodeThornOptions& ctOpt, SgProject* sageProject);
    void optionallyRunIOSequenceGenerator(CodeThornOptions& ctOpt, IOAnalyzer* analyzer);
    void optionallyAnnotateTermsAndUnparse(CodeThornOptions& ctOpt, SgProject* sageProject, CTAnalysis* analyzer);
    void optionallyRunDataRaceDetection(CodeThornOptions& ctOpt, CTAnalysis* analyzer);
    void optionallyPrintProgramInfos(CodeThornOptions& ctOpt, CTAnalysis* analyzer);
    void optionallyRunNormalization(CodeThornOptions& ctOpt,SgProject* sageProject, TimingCollector& timingCollector);
    void setAssertConditionVariablesInAnalyzer(SgNode* root,CTAnalysis* analyzer);
    void optionallyEliminateRersArraysAndExit(CodeThornOptions& ctOpt, SgProject* sageProject, CTAnalysis* analyzer);
    void optionallyWriteSVCompWitnessFile(CodeThornOptions& ctOpt, CTAnalysis* analyzer);
    void optionallyAnalyzeAssertions(CodeThornOptions& ctOpt, LTLOptions& ltlOpt, IOAnalyzer* analyzer, TimingCollector& tc);
    void exprEvalTest(int argc, char* argv[],CodeThornOptions& ctOpt);

    IOAnalyzer* createAnalyzer(CodeThornOptions& ctOpt, LTLOptions& ltlOpt);
    IOAnalyzer* createEStateAnalyzer(CodeThornOptions& ctOpt, LTLOptions& ltlOpt, Labeler* labeler, VariableIdMappingExtended* vid, CFAnalysis* cfAnalysis, Solver* solver);
    //void runSolver(CodeThornOptions& ctOpt,CTAnalysis* analyzer, SgProject* sageProject,TimingCollector& tc);

    void optionallyGenerateVerificationReports(CodeThornOptions& ctOpt,CTAnalysis* analyzer);
    void optionallyGenerateCallGraphDotFile(CodeThornOptions& ctOpt,CTAnalysis* analyzer);
  
    SgProject* runRoseFrontEnd(int argc, char * argv[], CodeThornOptions& ctOpt, TimingCollector& timingCollector);
    void normalizationPass(CodeThornOptions& ctOpt, SgProject* sageProject);
    Labeler* createLabeler(SgProject* sageProject, VariableIdMappingExtended* variableIdMapping);
    VariableIdMappingExtended* createVariableIdMapping(CodeThornOptions& ctOpt, SgProject* sageProject);
    void optionallyPrintRunTimeAndMemoryUsage(CodeThornOptions& ctOpt,TimingCollector& tc);

  } // end of namespace CodeThornLib
    
} // end of namespace CodeThorn

#endif

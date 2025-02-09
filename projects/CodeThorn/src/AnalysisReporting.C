#include "sage3basic.h"
#include "AnalysisReporting.h"
#include <iostream>
#include <string>
#include "CTAnalysis.h"
#include "AstStatistics.h"
#include "CppStdUtilities.h"
#include "ProgramLocationsAnalysis.h"
#include "BoolLattice.h"
#include "ConstantConditionAnalysis.h"
#include <climits>

using namespace std;
using namespace CodeThorn;

namespace CodeThorn {

  LabelSet AnalysisReporting::functionLabels(CodeThorn::CTAnalysis* analyzer) {
    LabelSet allFunctionLabels;
    LabelSet functionEntryLabels=analyzer->functionEntryLabels();
    Flow& flow=*analyzer->getFlow();
    for(auto entryLabel : functionEntryLabels) {
      LabelSet funLabSet=analyzer->getCFAnalyzer()->functionLabelSet(entryLabel,flow);
      allFunctionLabels+=funLabSet;
    }
    return allFunctionLabels;
  }

  bool AnalysisReporting::isSystemHeaderLabel(CodeThorn::CTAnalysis* analyzer, Label lab) {
    ROSE_ASSERT(analyzer->getLabeler());
    SgNode* node=analyzer->getLabeler()->getNode(lab);
    if(SgLocatedNode* locNode=isSgLocatedNode(node)) {
      return SageInterface::insideSystemHeader(locNode);
    } else {
      return false;
    }
  }
  
  void AnalysisReporting::printSeparationLine() {
    cout<<"------------------------------------------------"<<endl;
  }

  void AnalysisReporting::generateVerificationReports(CodeThornOptions& ctOpt, CodeThorn::CTAnalysis* analyzer, bool reportDetectedErrorLines) {
    for(auto analysisInfo : ctOpt.analysisList()) {
      AnalysisSelector analysisSel=analysisInfo.first;
      string analysisName=analysisInfo.second;
      if(ctOpt.getAnalysisSelectionFlag(analysisSel)) {
        cout<<endl;
        printSeparationLine();
        cout<<"Analysis results for "<<analysisName<<" analysis:"<<endl;
        printSeparationLine();
        ProgramLocationsReport report=analyzer->getEStateTransferFunctions()->getProgramLocationsReport(analysisSel);
        LabelSet labelsOfInterest2=AnalysisReporting::functionLabels(analyzer);
        // compute partioning
        LabelSet reachableLabels;
        LabelSet unreachableLabels;
        for(auto lab : labelsOfInterest2) {
          if(analyzer->isUnreachableLabel(lab)) {
            unreachableLabels.insert(lab);
          } else {
            reachableLabels.insert(lab);
          }
        }

        report.setReachableLocations(reachableLabels);
        report.setUnreachableLocations(unreachableLabels);

        switch(analysisSel) {
        case ANALYSIS_NULL_POINTER:
        case ANALYSIS_OUT_OF_BOUNDS:
        case ANALYSIS_UNINITIALIZED:
          report.writeLocationsVerificationReport(cout,analyzer->getLabeler());
          printSeparationLine();
          // generate verification call graph
          AnalysisReporting::generateVerificationFunctionsCsvFile(ctOpt,analyzer,analysisName,report,true);
          AnalysisReporting::generateVerificationCallGraphDotFile(ctOpt,analyzer,analysisName,report);
          //report->writeFunctionsVerificationReport(cout,analyzer->getLabeler());
          printSeparationLine();
          if(reportDetectedErrorLines) {
            LabelSet violatingLabels=report.falsifiedLocations();
            if(report.numDefinitiveLocations()>0) {
              cout<<"Proven errors in program:"<<endl;
              report.writeAllDefinitiveLocationsToStream(cout,analyzer->getLabeler(),false,true,true);
            } else {
              LabelSet u=report.unverifiedLocations();
              if(u.size()==0) {
                cout<<"No violations exist - program verified."<<endl;
              } else {
                cout<<"No violations proven - violations may exist in unverified portion of program."<<endl;
              }
            }
          }
          printSeparationLine();
          break;
        case ANALYSIS_DEAD_CODE:
          AnalysisReporting::generateDeadCodeLocationsVerificationReport(ctOpt, analyzer, unreachableLabels);
          AnalysisReporting::generateVerificationFunctionsCsvFile(ctOpt,analyzer,analysisName,report,false);
          AnalysisReporting::generateVerificationCallGraphDotFile(ctOpt,analyzer,analysisName,report);
          printSeparationLine();
          break;
        case ANALYSIS_OPAQUE_PREDICATE:
          generateConstantConditionVerificationReport(ctOpt, analyzer,analysisSel);
          break;
        default:
          cout<<"Error: generateVerificationReports: unknown analysis: "<<analysisSel<<endl;
          exit(1);
        }
      }
    }
  }

  void AnalysisReporting::generateDeadCodeLocationsVerificationReport(CodeThornOptions& ctOpt, CodeThorn::CTAnalysis* analyzer, LabelSet& unreachable) {
    if(ctOpt.deadCodeAnalysisFileName.size()>0) {
      stringstream locationsCSVFileData;
      for(auto lab : unreachable) {
        ROSE_ASSERT(analyzer->getLabeler());
        SgNode* node=analyzer->getLabeler()->getNode(lab);
        if(node) {
          //cout<<lab.toString()<<","<<value<<endl;
          locationsCSVFileData<<ProgramLocationsReport::programLocation(analyzer->getLabeler(),lab);
          // CONTINUE
        } else {
          locationsCSVFileData<<"unknown-location"<<endl;
        }
        locationsCSVFileData<<endl;
      }
      string s=locationsCSVFileData.str();
      if(!CppStdUtilities::writeFile(ctOpt.csvReportModeString,ctOpt.deadCodeAnalysisFileName, s)) {
        cerr<<"Error: cannot write file "<<ctOpt.deadCodeAnalysisFileName<<endl;
      } else {
        if(!ctOpt.quiet)
          cout<<"Generated analysis results in file "<<ctOpt.deadCodeAnalysisFileName<<endl;
      }
    }
  }

  void AnalysisReporting::generateConstantConditionVerificationReport(CodeThornOptions& ctOpt, CodeThorn::CTAnalysis* analyzer, AnalysisSelector analysisSel) {
    ROSE_ASSERT(analyzer->getEStateTransferFunctions());
    if(ReadWriteListener* readWriteListener=analyzer->getEStateTransferFunctions()->getReadWriteListener()) {
      ROSE_ASSERT(readWriteListener);
      ConstantConditionAnalysis* constCondAnalysis=dynamic_cast<ConstantConditionAnalysis*>(readWriteListener);
      ROSE_ASSERT(constCondAnalysis);
      ConstantConditionAnalysis::ConstConditionsMap& map=*constCondAnalysis->getResultMapPtr();
      stringstream locationsCSVFileData;
      std::uint32_t constTrueCnt=0;
      std::uint32_t constFalseCnt=0;
      for(auto p : map) {
        Label lab=p.first;
        BoolLattice value=p.second;
        if(value.isTop()) {
          continue;
        }
        ROSE_ASSERT(analyzer->getLabeler());
        SgNode* node=analyzer->getLabeler()->getNode(lab);
        SgNode* parent=node->get_parent();
        if(isSgWhileStmt(parent))
          continue;
        if(value.isTrue()) {
          constTrueCnt++;
          locationsCSVFileData<<"opaque-predicate,";
          locationsCSVFileData<<"true,";
        } else if(value.isFalse()){
          constFalseCnt++;
          locationsCSVFileData<<"opaque-predicate,";
          locationsCSVFileData<<"false,";
        } else {
          // bot?
          continue;
        }
        if(node) {
          //cout<<lab.toString()<<","<<value<<endl;
          locationsCSVFileData<<ProgramLocationsReport::programLocation(analyzer->getLabeler(),lab);
        } else {
          locationsCSVFileData<<"unknown-location"<<endl;
        }
        locationsCSVFileData<<endl;
      }
      cout<<"constant true        locations: "<<setw(6)<<constTrueCnt<<endl;
      cout<<"constant false       locations: "<<setw(6)<<constFalseCnt<<endl;
      printSeparationLine();
      string fileName=ctOpt.getAnalysisReportFileName(analysisSel);
      if(fileName.size()>0) {
        if(!CppStdUtilities::writeFile(ctOpt.csvReportModeString,fileName, locationsCSVFileData.str())) {
          cerr<<"Error: cannot write file "<<fileName<<endl;
        } else {
          if(!ctOpt.quiet)
            cout<<"Generated analysis results in file "<<fileName<<endl;
        }
        
      }
      
    } else {
      cout<<"WARNING: Opaque Predicate Analysis report generation: plug-in not registered."<<endl;
    }
  }
  
  void AnalysisReporting::generateAnalysisStatsRawData(CodeThornOptions& ctOpt, CodeThorn::CTAnalysis* analyzer) {
    for(auto analysisInfo : ctOpt.analysisList()) {
      AnalysisSelector analysisSel=analysisInfo.first;
      // exception: skip some analysis, because they use their own format
      if(analysisSel==ANALYSIS_OPAQUE_PREDICATE||analysisSel==ANALYSIS_DEAD_CODE)
        continue;
      string analysisName=analysisInfo.second;
      if(ctOpt.getAnalysisReportFileName(analysisSel).size()>0) {
        ProgramLocationsReport locations=analyzer->getEStateTransferFunctions()->getProgramLocationsReport(analysisSel);
        string fileName=ctOpt.getAnalysisReportFileName(analysisSel);
        if(!ctOpt.quiet)
          cout<<"Writing "<<analysisName<<" analysis results to file "<<fileName<<endl;
        locations.writeResultFile(fileName,ctOpt.csvReportModeString,analyzer->getLabeler());
      }
    }
  }
                             
  void AnalysisReporting::generateNullPointerAnalysisStats(CodeThorn::CTAnalysis* analyzer) {
    ProgramLocationsAnalysis pla;
    LabelSet pdlSet=pla.pointerDereferenceLocations(*analyzer->getLabeler());
    cout<<"Found "<<pdlSet.size()<<" pointer dereference locations."<<endl;
    
  }
  void AnalysisReporting::generateAstNodeStats(CodeThornOptions& ctOpt, SgProject* sageProject) {
    if(ctOpt.info.printAstNodeStats||ctOpt.info.astNodeStatsCSVFileName.size()>0) {
      // from: src/midend/astDiagnostics/AstStatistics.C
      if(ctOpt.info.printAstNodeStats) {
        ROSE_Statistics::AstNodeTraversalStatistics astStats;
        string s=astStats.toString(sageProject);
        cout<<s; // output includes newline at the end
      }
      if(ctOpt.info.astNodeStatsCSVFileName.size()>0) {
        ROSE_Statistics::AstNodeTraversalCSVStatistics astCSVStats;
        string fileName=ctOpt.info.astNodeStatsCSVFileName;
        astCSVStats.setMinCountToShow(1); // default value is 1
        if(!CppStdUtilities::writeFile(fileName, astCSVStats.toString(sageProject))) {
          cerr<<"Error: cannot write AST node statistics to CSV file "<<fileName<<endl;
        }
      }
    }
  }

  void AnalysisReporting::generateAnalyzedFunctionsAndFilesReports(CodeThornOptions& ctOpt, CodeThorn::CTAnalysis* analyzer) {
    if(ctOpt.analyzedFunctionsCSVFileName.size()>0) {
      string fileName=ctOpt.analyzedFunctionsCSVFileName;
      if(!ctOpt.quiet)
        cout<<"Writing list of analyzed functions to file "<<fileName<<endl;
      string s=analyzer->analyzedFunctionsToString();
      if(!CppStdUtilities::writeFile(fileName, s)) {
        cerr<<"Cannot create file "<<fileName<<endl;
        exit(1);
      }
    }

    if(ctOpt.analyzedFilesCSVFileName.size()>0) {
      string fileName=ctOpt.analyzedFilesCSVFileName;
      if(!ctOpt.quiet)
        cout<<"Writing list of analyzed files to file "<<fileName<<endl;
      string s=analyzer->analyzedFilesToString();
      if(!CppStdUtilities::writeFile(fileName, s)) {
        cerr<<"Cannot create file "<<fileName<<endl;
        exit(1);
      }
    }

    if(ctOpt.externalFunctionCallsCSVFileName.size()>0) {
      string fileName=ctOpt.externalFunctionsCSVFileName;
      if(!ctOpt.quiet)
        cout<<"Writing list of external function calls to file "<<fileName<<endl;
      string s=analyzer->externalFunctionsToString();
      if(!CppStdUtilities::writeFile(fileName, s)) {
        cerr<<"Cannot create file "<<fileName<<endl;
        exit(1);
      }
    }
  }

  void AnalysisReporting::generateVerificationCallGraphDotFile(CodeThornOptions& ctOpt, CodeThorn::CTAnalysis* analyzer, string analysisName, ProgramLocationsReport& report) {
    string fileName1=analysisName+"-cg1.dot";
    string fileName2=analysisName+"-cg2.dot";
    //cout<<"Generating verification call graph for "<<analysisName<<" analysis."<<endl;
    Flow& flow=*analyzer->getFlow();
    LabelSet functionEntryLabels=analyzer->getCFAnalyzer()->functionEntryLabels(flow);
    std::map<Label,VerificationResult> fMap;

    calculatefMap(fMap,analyzer,functionEntryLabels,flow,report);

    InterFlow::LabelToFunctionMap map=analyzer->getCFAnalyzer()->labelToFunctionMap(flow);

    std::string cgBegin="digraph G {\n";
    std::string cgEnd="}\n";
    std::string cgEdges=analyzer->getInterFlow()->dotCallGraphEdges(map);
    // generate colored nodes
    std::string nodeColor;
    stringstream cgNodes;
    int numFalsifiedFunctions=0;
    int numUnverifiedFunctions=0;
    int numVerifiedFunctions=0;
    for (auto entryLabel : functionEntryLabels ) {
      switch(fMap[entryLabel]) {
      case FALSIFIED: nodeColor="red";numFalsifiedFunctions++;break;
      case UNVERIFIED: nodeColor="orange";numUnverifiedFunctions++;break;
      case VERIFIED: nodeColor="green";numVerifiedFunctions++;break;
      case INCONSISTENT: nodeColor="orchid1";break;
      case UNREACHABLE: nodeColor="gray";break;
      }
      std::string functionName=SgNodeHelper::getFunctionName(analyzer->getLabeler()->getNode(entryLabel));
      std::string dotFunctionName="label=\""+entryLabel.toString()+":"+functionName+"\"";
      //if(nodeColor!="gray")
        cgNodes<<entryLabel.toString()<<" [style=filled, fillcolor="<<nodeColor<<","<<dotFunctionName<<"]"<<endl;
    }

    // without function name, only label id
    stringstream cgNodes2;
    for (auto entryLabel : functionEntryLabels ) {
      switch(fMap[entryLabel]) {
      case FALSIFIED: nodeColor="red";break;
      case UNVERIFIED: nodeColor="orange";break;
      case VERIFIED: nodeColor="green";break;
      case INCONSISTENT: nodeColor="orchid1";break;
      case UNREACHABLE: nodeColor="gray";break;
      }
      std::string functionName=SgNodeHelper::getFunctionName(analyzer->getLabeler()->getNode(entryLabel));
      std::string dotFunctionName="label=\""+entryLabel.toString()+"\"";
      if(nodeColor!="orchid1")
        cgNodes2<<entryLabel.toString()<<" [style=filled, fillcolor="<<nodeColor<<","<<dotFunctionName<<"]"<<endl;
    }

    std::string dotFileString1=cgBegin+cgNodes2.str()+cgEdges+cgEnd;
    if(!CppStdUtilities::writeFile(fileName1, dotFileString1)) {
      cerr<<"Error: could not generate callgraph dot file "<<fileName1<<endl;
      exit(1);
    } else {
      cout<<"Generated verification call graph "<<fileName1<<endl;
    }

    std::string dotFileString2=cgBegin+cgNodes.str()+cgEdges+cgEnd;
    if(!CppStdUtilities::writeFile(fileName2, dotFileString2)) {
      cerr<<"Error: could not generate callgraph dot file "<<fileName2<<endl;
      exit(1);
    } else {
      cout<<"Generated verification call graph "<<fileName2<<endl;
    }

  }

  void AnalysisReporting::calculatefMap(std::map<Label,VerificationResult>& fMap,CTAnalysis* analyzer, LabelSet& functionEntryLabels, Flow& flow, ProgramLocationsReport& report) {
    LabelSet verified=report.verifiedLocations();
    LabelSet falsified=report.falsifiedLocations();
    LabelSet unverified=report.unverifiedLocations();

    for(auto entryLabel : functionEntryLabels) {
      if(analyzer->isUnreachableLabel(entryLabel)) {
        fMap[entryLabel]=UNREACHABLE;
        continue;
      }
      LabelSet funLabSet=analyzer->getCFAnalyzer()->functionLabelSet(entryLabel,flow);
      //cout<<"Function label set:"<<funLabSet.toString()<<endl;
      //for(auto lab : funLabSet) {
      //  cout<<lab.toString()<<":"<<analyzer->getLabeler()->getNode(lab)->class_name()<<endl;
      //}
      VerificationResult funVer=INCONSISTENT;
      size_t count=0;
      size_t unreachableCount=0;
      for(auto lab : funLabSet) {
        if(falsified.isElement(lab)) {
          funVer=FALSIFIED;
          break;
        } else if(analyzer->isUnreachableLabel(lab)) {
          unreachableCount++;
        } else if(unverified.isElement(lab)) {
          funVer=UNVERIFIED;
        } else if(verified.isElement(lab)) {
          count++;
        }
      }
      //cout<<"DEBUG: Function verification: "<<SgNodeHelper::getFunctionName(analyzer->getLabeler()->getNode(entryLabel))<<":"<<funLabSet.size()<<" vs "<<count<<" + "<<unreachableCount<<endl;
      if(funLabSet.size()==count+unreachableCount) {
        funVer=VERIFIED;
      }
      fMap[entryLabel]=funVer;
      //cout<<"DEBUG:entrylabel:"<<entryLabel.toString()<<" : "<<funVer<<" : "<< analyzer->getLabeler()->getNode(entryLabel)->unparseToString()<<endl;
    }
  }
  
  void AnalysisReporting::generateVerificationFunctionsCsvFile(CodeThornOptions& ctOpt, CodeThorn::CTAnalysis* analyzer, string analysisName, ProgramLocationsReport& report, bool violationReporting) {
    string fileName1=analysisName+"-functions.csv";
    
    Flow& flow=*analyzer->getFlow();
    std::map<Label,VerificationResult> fMap;
    LabelSet functionEntryLabels=analyzer->getCFAnalyzer()->functionEntryLabels(flow);

    calculatefMap(fMap,analyzer,functionEntryLabels,flow,report);

    InterFlow::LabelToFunctionMap map=analyzer->getCFAnalyzer()->labelToFunctionMap(flow);
    stringstream cgNodes;
    string csvEntryType;
    int numFalsifiedFunctions=0;
    int numUnverifiedFunctions=0;
    int numVerifiedFunctions=0;
    int numInconsistentFunctions=0;
    int numUnreachableFunctions=0;
    for (auto entryLabel : functionEntryLabels ) {
      switch(fMap[entryLabel]) {
      case FALSIFIED: csvEntryType="violated";numFalsifiedFunctions++;break;
      case UNVERIFIED: csvEntryType="undecided";numUnverifiedFunctions++;break;
      case VERIFIED: csvEntryType="verified";numVerifiedFunctions++;break;
      case INCONSISTENT: csvEntryType="inconsistent";numInconsistentFunctions++;break;
      case UNREACHABLE: csvEntryType="dead";numUnreachableFunctions++;break;
      }
      SgNode* node=analyzer->getLabeler()->getNode(entryLabel);
      string fileName=SgNodeHelper::sourceFilenameToString(node);
      std::string functionName=SgNodeHelper::getFunctionName(node);
      //      if(csvEntryType!="inconsistent")
      if(analysisName!="dead-code") {
        cgNodes<<fileName<<","<<functionName<<","<<csvEntryType<<endl;
      } else {
        if(csvEntryType=="dead")
          cgNodes<<fileName<<","<<functionName<<","<<csvEntryType<<endl;
      }
    }

    // print stats
    int numProvenFunctions=numVerifiedFunctions+numFalsifiedFunctions;
    int numTotalReachableFunctions=numProvenFunctions+numUnverifiedFunctions+numInconsistentFunctions;
    int numTotalUnreachableFunctions=numUnreachableFunctions;
    int numTotalFunctions=numTotalReachableFunctions+numTotalUnreachableFunctions;
    //    cout<<"Reachable verified   functions: "<<numProvenFunctions<<" [ "<<numProvenFunctions/(double)numTotalReachableFunctions*100<<"%]"<<endl;
    if(violationReporting) {
      cout<<"Verified  (definitely safe)    functions: "<<setw(6)<<numVerifiedFunctions<<" ["<<setw(6)<<numVerifiedFunctions/(double)numTotalReachableFunctions*100<<"%]"<<endl;
      cout<<"Violated  (definitely unsafe)  functions: "<<setw(6)<<numFalsifiedFunctions<<" ["<<setw(6)<<numFalsifiedFunctions/(double)numTotalReachableFunctions*100<<"%]"<<endl ;
      cout<<"Undecided (potentially unsafe) functions: "<<setw(6)<<numUnverifiedFunctions<<" ["<<setw(6)<<numUnverifiedFunctions/(double)numTotalReachableFunctions*100<<"%]"<<endl;
    //cout<<"Total reachable      functions: "<<setw(6)<<numTotalReachableFunctions<<" ["<<setw(6)<<numTotalReachableFunctions/(double)numTotalFunctions*100<<"%]"<<endl;
    }
    cout<<"Dead (unreachable)             functions: "<<setw(6)<<numTotalUnreachableFunctions<<" ["<<setw(6)<<numTotalUnreachableFunctions/(double)numTotalFunctions*100<<"%]"<<endl;
    cout<<"Total                          functions: "<<setw(6)<<numTotalFunctions<<endl;
    if(numInconsistentFunctions>0)
      cout<<"Inconsistent                   functions: "<<setw(6)<<numInconsistentFunctions<<" ["<<setw(6)<<numInconsistentFunctions/(double)numTotalUnreachableFunctions*100<<"%]"<<endl;
    
    printSeparationLine();
    std::string dotFileString1=cgNodes.str();
    if(!CppStdUtilities::writeFile(ctOpt.csvReportModeString, fileName1, dotFileString1)) {
      cerr<<"Error: could not generate function verification file "<<fileName1<<endl;
      exit(1);
    } else {
      cout<<"Generated function verification file "<<fileName1<<endl;
    }

  }
  
} // end of namespace CodeThorn

#include "sage3basic.h"
#include "Solver16.h"
#include "CTAnalysis.h"
#include "CodeThornCommandLineOptions.h"
#include "EStateTransferFunctions.h"

using namespace std;
using namespace CodeThorn;
using namespace Sawyer::Message;

#include "CTAnalysis.h"

Sawyer::Message::Facility Solver16::logger;
// initialize static member flag
bool Solver16::_diagnosticsInitialized = false;

Solver16::Solver16() {
  initDiagnostics();
}

int Solver16::getId() {
  return 16;
}
    
void Solver16::recordTransition(const EState* currentEStatePtr0,const EState* currentEStatePtr,Edge e, const EState* newEStatePtr) {
  _analyzer->recordTransition(currentEStatePtr,e,newEStatePtr);
  if(currentEStatePtr0!=currentEStatePtr) {
    // also add transition edge for the state from
    // worklist if it is different to the summary state
    // (to which an edge must exist in the STS)
    Edge e0(currentEStatePtr0->label(),e.getTypes(),newEStatePtr->label());
    e0.setAnnotation(e.getAnnotation());
    _analyzer->recordTransition(currentEStatePtr0,e0,newEStatePtr);
  }
}

void Solver16::initializeSummaryStatesFromWorkList() {
  // pop all states from worklist (can contain more than one state)
  list<const EState*> tmpWL;
  while(!_analyzer->isEmptyWorkList()) {
    tmpWL.push_back(_analyzer->popWorkList());
  }
  for(auto s : tmpWL) {
    // initialize summarystate and push back to work list
    _analyzer->setSummaryState(s->label(),s->callString,s);
    _analyzer->addToWorkList(s);
  }
}


/*! 
  * \author Markus Schordan
  * \date 2012.
 */
void Solver16::run() {
  logger[INFO]<<"Running solver "<<getId()<<endl;
  if(_analyzer->getOptionsRef().abstractionMode==0) {
    cerr<<"Error: abstraction mode is 0, but >= 1 required."<<endl;
    exit(1);
  }
  if(_analyzer->getOptionsRef().explorationMode!="topologic-sort") {
    cerr<<"Error: topologic-sort required for exploration mode, but it is "<<_analyzer->getOptionsRef().explorationMode<<endl;
    exit(1);
  }

  initializeSummaryStatesFromWorkList();

  if(_analyzer->svCompFunctionSemantics()) {
    _analyzer->reachabilityResults.init(1); // in case of svcomp mode set single program property to unknown
  } else {
    _analyzer->reachabilityResults.init(_analyzer->getNumberOfErrorLabels()); // set all reachability results to unknown
  }
  logger[INFO]<<"number of error labels: "<<_analyzer->reachabilityResults.size()<<endl;
  size_t prevStateSetSize=0; // force immediate report at start
  int threadNum;
  int workers=_analyzer->getOptionsRef().threads;
  vector<bool> workVector(workers);
  _analyzer->set_finished(workVector,true);
  bool terminateEarly=false;
  //omp_set_dynamic(0);     // Explicitly disable dynamic teams
  omp_set_num_threads(workers);

  bool ioReductionActive = false;
  unsigned int ioReductionThreshold = 0;
  unsigned int estatesLastReduction = 0;
  if(_analyzer->getLtlOptionsRef().ioReduction) {
    ioReductionActive = true;
    ioReductionThreshold = _analyzer->getLtlOptionsRef().ioReduction;
  }

  SAWYER_MESG(logger[TRACE])<<"STATUS: Running parallel solver "<<getId()<<" with "<<workers<<" threads."<<endl;
  _analyzer->printStatusMessage(true);
# pragma omp parallel shared(workVector) private(threadNum)
  {
    threadNum=omp_get_thread_num();
    while(!_analyzer->all_false(workVector)) {
      // logger[DEBUG]<<"running : WL:"<<estateWorkListCurrent->size()<<endl;
      if(threadNum==0 && _analyzer->getOptionsRef().displayDiff && (_analyzer->getEStateSetSize()>(prevStateSetSize+_analyzer->getOptionsRef().displayDiff))) {
        _analyzer->printStatusMessage(true);
        prevStateSetSize=_analyzer->getEStateSetSize();
      }
      //perform reduction to I/O/worklist states only if specified threshold was reached
      if (ioReductionActive) {
#pragma omp critical
        {
          if (_analyzer->getEStateSetSize() > (estatesLastReduction + ioReductionThreshold)) {
            _analyzer->reduceStgToInOutAssertWorklistStates();
            estatesLastReduction = _analyzer->getEStateSetSize();
            cout<< "STATUS: transition system reduced to I/O/worklist states. remaining transitions: " << _analyzer->getTransitionGraphSize() << endl;
          }
        }
      }
      if(_analyzer->isEmptyWorkList()||_analyzer->isIncompleteSTGReady()) {
#pragma omp critical
        {
          workVector[threadNum]=false;
        }
        continue;
      } else {
#pragma omp critical
        {
          if(terminateEarly)
            workVector[threadNum]=false;
          else
            workVector[threadNum]=true;
        }
      }
      // currentEStatePtr0 is not merged, because it must already be present in a summary state. Here only the (label,callstring) is used to obtain the summary state.
      // the worklist could be reduced to (label,callstring) pairs, but since it's also used for explicit model checking, it uses pointers to estates, which include some more info.
      // note: initial summary states are set in initializeSummaryStatesFromWorkList()
      const EState* currentEStatePtr0=_analyzer->popWorkList();
      // difference to Solver5: always obtain abstract state
      const EState* currentEStatePtr=_analyzer->getSummaryState(currentEStatePtr0->label(),currentEStatePtr0->callString);
      if(currentEStatePtr0!=currentEStatePtr) {
        //cout<<"DEBUG: deleting obsolete state "<<currentEStatePtr0<<endl;
        //delete currentEStatePtr0; // can be deleted because it is no longer used, instead the summary state is used
      }
      // if we want to terminate early, we ensure to stop all threads and empty the worklist (e.g. verification error found).
      if(terminateEarly)
        continue;
      if(!currentEStatePtr) {
        //cerr<<"Thread "<<threadNum<<" found empty worklist. Continue without work. "<<endl;
        ROSE_ASSERT(threadNum>=0 && threadNum<=_analyzer->getOptionsRef().threads);
      } else {
        ROSE_ASSERT(currentEStatePtr);
        Flow edgeSet=_analyzer->getFlow()->outEdges(currentEStatePtr->label());
        //cout << "DEBUG: out-edgeSet size:"<<edgeSet.size()<<endl;
        for(Flow::iterator i=edgeSet.begin();i!=edgeSet.end();++i) {
          Edge e=*i;
          list<EState> newEStateList;
          newEStateList=_analyzer->transferEdgeEState(e,currentEStatePtr);
          for(list<EState>::iterator nesListIter=newEStateList.begin();
              nesListIter!=newEStateList.end();
              ++nesListIter) {
            // newEstate is passed by value (not created yet)
            EState newEState=*nesListIter;
            ROSE_ASSERT(newEState.label()!=Labeler::NO_LABEL);
            if(_analyzer->getOptionsRef().stgTraceFileName.size()>0 && !newEState.constraints()->disequalityExists()) {
              std::ofstream fout;
#pragma omp critical
              {
                fout.open(_analyzer->getOptionsRef().stgTraceFileName.c_str(),ios::app);    // open file for appending
                assert (!fout.fail( ));
                fout<<"ESTATE-IN :"<<currentEStatePtr->toString(_analyzer->getVariableIdMapping());
                string sourceString=_analyzer->getCFAnalyzer()->getLabeler()->getNode(currentEStatePtr->label())->unparseToString().substr(0,40);
                if(sourceString.size()==60) sourceString+="...";
                fout<<"\n==>"<<"TRANSFER:"<<sourceString;
                fout<<"==>\n"<<"ESTATE-OUT:"<<newEState.toString(_analyzer->getVariableIdMapping());
                fout<<endl;
                fout<<endl;
                fout.close();
              }
            }
            
            if((!newEState.constraints()->disequalityExists()) &&(!_analyzer->isFailedAssertEState(&newEState)&&!_analyzer->isVerificationErrorEState(&newEState))) {
              HSetMaintainer<EState,EStateHashFun,EStateEqualToPred>::ProcessingResult pres=_analyzer->process(newEState);
              const EState* newEStatePtr=pres.second;
              if(pres.first==true) {
                int abstractionMode=_analyzer->getAbstractionMode();
                switch(abstractionMode) {
                case 1:
                  {
                  // performing merge
#pragma omp critical(SUMMARY_STATES_MAP)
                  {
                    const EState* summaryEState=_analyzer->getSummaryState(newEStatePtr->label(),newEStatePtr->callString);
                    if(_analyzer->getEStateTransferFunctions()->isApproximatedBy(newEStatePtr,summaryEState)) {
                      // this is not a memory leak. newEStatePtr is
                      // stored in EStateSet and will be collected
                      // later. It may be already used in the state
                      // graph as an existing estate.
                      newEStatePtr=summaryEState; 
                    } else {
                      stringstream condss;
                      EState newEState2=_analyzer->getEStateTransferFunctions()->combine(summaryEState,const_cast<EState*>(newEStatePtr));
                      ROSE_ASSERT(_analyzer);
                      HSetMaintainer<EState,EStateHashFun,EStateEqualToPred>::ProcessingResult pres=_analyzer->process(newEState2);
                      const EState* newEStatePtr2=pres.second;

                      // DEBUG
#if 0
                      int checkId=220;
                      int id=newEStatePtr2->label().getId();
                      if(id==checkId) {
                        cout<<"--------------------------------------------------"<<endl;
                        cout<<"@"<<id<<": APPROX-BY-1:"<<newEStatePtr->toString()<<endl;
                        cout<<"@"<<id<<": APPROX-BY-2:"<<summaryEState->toString()<<endl;
                        cout<<"@"<<id<<": MERGED     :"<<newEStatePtr2->toString()<<endl;
                      }
#endif
                      
                      if(pres.first==true) {
                        newEStatePtr=newEStatePtr2;
                      } else {
                        // nothing to do, EState already exists
                      }
                      ROSE_ASSERT(newEStatePtr);
                      _analyzer->setSummaryState(newEStatePtr->label(),newEStatePtr->callString,newEStatePtr);
#if 0
                      if(id==checkId) {
                        cout<<"@"<<id<<": MERGED SUM :"<<_analyzer->getSummaryState(newEStatePtr->label(),newEStatePtr->callString)->toString()<<endl;
                        cout<<"--------------------------------------------------"<<endl;
                      }
#endif
                    }
                  }
                  _analyzer->addToWorkList(newEStatePtr);  
                  break;
                  case 2: 
                    cerr<<"Error: abstraction mode 2 not suppored in solver 16."<<endl;
                    exit(1);
                }
                default:
                  cerr<<"Error: unknown abstraction mode "<<abstractionMode<<endl;
                  exit(1);
                }
              } else {
                //cout<<"DEBUG: pres.first==false (not adding estate to worklist)"<<endl;
              }
              recordTransition(currentEStatePtr0,currentEStatePtr,e,newEStatePtr);
            }
            if((!newEState.constraints()->disequalityExists()) && ((_analyzer->isFailedAssertEState(&newEState))||_analyzer->isVerificationErrorEState(&newEState))) {
              // failed-assert end-state: do not add to work list but do add it to the transition graph
              const EState* newEStatePtr;
              newEStatePtr=_analyzer->processNewOrExisting(newEState);
              recordTransition(currentEStatePtr0,currentEStatePtr,e,newEStatePtr);

              if(_analyzer->isVerificationErrorEState(&newEState)) {
#pragma omp critical
                {
                  SAWYER_MESG(logger[TRACE]) <<"STATUS: detected verification error state ... terminating early"<<endl;
                  // set flag for terminating early
                  _analyzer->reachabilityResults.reachable(0);
                  _analyzer->_firstAssertionOccurences.push_back(pair<int, const EState*>(0, newEStatePtr));
                  terminateEarly=true;
                }
              } else if(_analyzer->isFailedAssertEState(&newEState)) {
                // record failed assert
                int assertCode;
                if(_analyzer->getOptionsRef().rers.rersBinary) {
                  assertCode=_analyzer->reachabilityAssertCode(newEStatePtr);
                } else {
                  assertCode=_analyzer->reachabilityAssertCode(currentEStatePtr);
                }
                if(assertCode>=0) {
#pragma omp critical
                  {
                    if(_analyzer->getLtlOptionsRef().withCounterExamples || _analyzer->getLtlOptionsRef().withAssertCounterExamples) {
                      //if this particular assertion was never reached before, compute and update counterexample
                      if (_analyzer->reachabilityResults.getPropertyValue(assertCode) != PROPERTY_VALUE_YES) {
                        _analyzer->_firstAssertionOccurences.push_back(pair<int, const EState*>(assertCode, newEStatePtr));
                      }
                    }
                    _analyzer->reachabilityResults.reachable(assertCode);
                  }
                }
              } // end of failed assert handling
            } // end of if (no disequality (= no infeasable path))
          } // end of loop on transfer function return-estates
        } // edge set iterator
      } // conditional: test if work is available
    } // while
  } // omp parallel
  const bool isComplete=true;
  if (!_analyzer->isPrecise()) {
    _analyzer->_firstAssertionOccurences = list<FailedAssertion>(); //ignore found assertions if the STG is not precise
  }
  if(_analyzer->isIncompleteSTGReady()) {
    _analyzer->printStatusMessage(true);
    _analyzer->printStatusMessage("STATUS: analysis finished (incomplete STG due to specified resource restriction).",true);
    _analyzer->reachabilityResults.finishedReachability(_analyzer->isPrecise(),!isComplete);
    _analyzer->getTransitionGraph()->setIsComplete(!isComplete);
  } else {
    bool tmpcomplete=true;
    _analyzer->reachabilityResults.finishedReachability(_analyzer->isPrecise(),tmpcomplete);
    _analyzer->printStatusMessage(true);
    _analyzer->getTransitionGraph()->setIsComplete(tmpcomplete);
    _analyzer->printStatusMessage("STATUS: analysis finished (worklist is empty).",true);
  }
  _analyzer->getTransitionGraph()->setIsPrecise(_analyzer->isPrecise());
}

void Solver16::initDiagnostics() {
  if (!_diagnosticsInitialized) {
    _diagnosticsInitialized = true;
    Solver::initDiagnostics(logger, getId());
  }
}

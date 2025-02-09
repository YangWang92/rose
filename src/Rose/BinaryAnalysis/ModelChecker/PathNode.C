#include <featureTests.h>
#ifdef ROSE_ENABLE_BINARY_ANALYSIS
#include <sage3basic.h>
#include <Rose/BinaryAnalysis/ModelChecker/PathNode.h>

#include <boost/scope_exit.hpp>
#include <Rose/BinaryAnalysis/ModelChecker/ExecutionUnit.h>
#include <Rose/BinaryAnalysis/ModelChecker/SemanticCallbacks.h>
#include <Rose/BinaryAnalysis/ModelChecker/Settings.h>
#include <Rose/BinaryAnalysis/ModelChecker/Tag.h>
#include <Rose/BinaryAnalysis/InstructionSemantics2/BaseSemantics/State.h>
#include <rose_isnan.h>

using namespace Sawyer::Message::Common;
namespace BS = Rose::BinaryAnalysis::InstructionSemantics2::BaseSemantics;

namespace Rose {
namespace BinaryAnalysis {
namespace ModelChecker {

PathNode::PathNode(const ExecutionUnit::Ptr &unit)
    : executionUnit_(unit), id_(Sawyer::fastRandomIndex(UINT64_MAX)) {
    ASSERT_not_null(unit);
}

PathNode::PathNode(const Ptr &parent, const ExecutionUnit::Ptr &unit, const SymbolicExpr::Ptr &assertion,
                   const BS::StatePtr &parentOutgoingState)
    : parent_(parent), executionUnit_(unit),  incomingState_(parentOutgoingState), assertions_{assertion},
      id_(Sawyer::fastRandomIndex(UINT64_MAX)) {
    ASSERT_not_null(unit);
    ASSERT_not_null(parent);
    ASSERT_not_null(assertion);
}

PathNode::~PathNode() {}

PathNode::Ptr
PathNode::instance(const ExecutionUnit::Ptr &unit) {
    ASSERT_not_null(unit);
    return Ptr(new PathNode(unit));
}

PathNode::Ptr
PathNode::instance(const Ptr &parent, const ExecutionUnit::Ptr &unit, const SymbolicExpr::Ptr &assertion,
                   const BS::StatePtr &parentOutgoingState) {
    ASSERT_not_null(unit);
    ASSERT_not_null(parent);
    ASSERT_not_null(assertion);
    return Ptr(new PathNode(parent, unit, assertion, parentOutgoingState));
}

uint64_t
PathNode::id() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return id_;
}

double
PathNode::sortKey() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return sortKey_;
}

void
PathNode::sortKey(double d) {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    sortKey_ = d;
}

PathNode::Ptr
PathNode::parent() const {
    // No lock necessary since this cannot change after object construction
    return parent_;
}

ExecutionUnit::Ptr
PathNode::executionUnit() const {
    // No lock necessary since this cannot change after object construction
    ASSERT_not_null(executionUnit_);
    return executionUnit_;
}

size_t
PathNode::nSteps() const {
    // No lock necessary since execution unit pointer can't change after object construction
    ASSERT_not_null(executionUnit_);
    return executionUnit_->nSteps();
}

void
PathNode::assertion(const SymbolicExpr::Ptr &expr) {
    ASSERT_not_null(expr);
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    assertions_.push_back(expr);
}

std::vector<SymbolicExpr::Ptr>
PathNode::assertions() const {
    std::vector<SymbolicExpr::Ptr> retval;
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return assertions_;                                 // must be a copy for thread safety
}

void
PathNode::execute(const Settings::Ptr &settings, const SemanticCallbacksPtr &semantics, const BS::RiscOperatorsPtr &ops,
                  const SmtSolver::Ptr &solver) {
    ASSERT_not_null(settings);
    ASSERT_not_null(semantics);
    ASSERT_not_null(ops);
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);

    // Do we actually need to execute anything?
    if (outgoingState_ || executionFailed_) {
        return;                                         // already executed
    } else if (settings->rejectUnknownInsns && executionUnit_->containsUnknownInsn()) {
        SAWYER_MESG(mlog[DEBUG]) <<"  contains not-allowed \"unknown\" instruction(s)\n";
        executionFailed_ = true;
        return;
    }

    // Get the incoming state, which might require recursively executing the parent.
    BS::StatePtr state;
    bool needsInitialization = false;
    if (incomingState_) {
        state = incomingState_->clone();
    } else if (parent_) {
        parent_->execute(settings, semantics, ops, solver);
        state = parent_->copyOutgoingState();
    } else {
        state = semantics->createInitialState();
        needsInitialization = true;
    }

    // Prepare the RISC operators and maybe initial the path initial state
    ASSERT_forbid(ops->currentState());                 // safety net
    ops->currentState(state);                       // ops is thread local, references state
    BOOST_SCOPE_EXIT(&ops) {
        ops->currentState(nullptr);
    } BOOST_SCOPE_EXIT_END;
    if (needsInitialization)
        semantics->initializeState(ops);

    // Allow the semantics layer to access the model checker's SMT solver if necessary.
    SmtSolver::Transaction tx(solver);
    semantics->attachModelCheckerSolver(ops, solver);
    BOOST_SCOPE_EXIT(&semantics, &ops) {
        semantics->attachModelCheckerSolver(ops, SmtSolver::Ptr());
    } BOOST_SCOPE_EXIT_END;

    // Execute the current node. Note that we're still holding the lock on this node so if other threads are also needing to
    // execute this node, they'll block and when they finally make progress they'll return fast (the "already executed"
    // condition above).
    if (ops->currentState()) {                          // null if parent execution failed
        SAWYER_MESG(mlog[DEBUG]) <<"  pre-execution semantics\n";
        std::vector<Tag::Ptr> tags = semantics->preExecute(executionUnit_, ops);
        tags_.insert(tags_.end(), tags.begin(), tags.end());
    }
    if (ops->currentState()) {
        std::vector<Tag::Ptr> tags = executionUnit_->execute(settings, semantics, ops);  // state is now updated
        tags_.insert(tags_.end(), tags.begin(), tags.end());
    }
    if (ops->currentState()) {
        SAWYER_MESG(mlog[DEBUG]) <<"  post-execution semantics\n";
        std::vector<Tag::Ptr> tags = semantics->postExecute(executionUnit_, ops);
        tags_.insert(tags_.end(), tags.begin(), tags.end());
    }

    // Execution has either succeeded or failed depending on whether the RISC operators has a current state.
    if (ops->currentState()) {
        outgoingState_ = state;
        executionFailed_ = false;
    } else {
        outgoingState_ = nullptr;
        executionFailed_ = true;
    }

    // We no longer need the incoming state
    incomingState_ = BS::StatePtr();

    // ops->currentState is reset to null on scope exit
}

bool
PathNode::executionFailed() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return executionFailed_;
}

BS::StatePtr
PathNode::copyOutgoingState() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return outgoingState_ ? outgoingState_->clone() : BS::StatePtr();
}

PathNode::BorrowedOutgoingState
PathNode::borrowOutgoingState() {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    ASSERT_not_null(outgoingState_);
    BorrowedOutgoingState borrowed(this, outgoingState_);
    outgoingState_ = BS::StatePtr();
    return borrowed;
}

void
PathNode::restoreOutgoingState(const BS::StatePtr &state) {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    ASSERT_forbid2(outgoingState_, "something gave this node a state while its state was borrowed");
    outgoingState_ = state;
}

PathNode::BorrowedOutgoingState::BorrowedOutgoingState(PathNode *node, const BS::StatePtr &state)
    : node(node), state(state) {
    ASSERT_not_null(node);
    ASSERT_not_null(state);
}

PathNode::BorrowedOutgoingState::~BorrowedOutgoingState() {
    ASSERT_not_null(node);
    ASSERT_not_null(state);
    node->restoreOutgoingState(state);
}

void
PathNode::releaseOutgoingState() {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    ASSERT_require(outgoingState_);
    outgoingState_ = BS::StatePtr();
}

void
PathNode::appendTag(const Tag::Ptr &tag) {
    ASSERT_not_null(tag);
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    tags_.push_back(tag);
}

void
PathNode::appendTags(const std::vector<Tag::Ptr> &tags) {
    if (!tags.empty()) {
        ASSERT_require(std::find(tags.begin(), tags.end(), Tag::Ptr()) == tags.end());
        SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
        tags_.insert(tags_.end(), tags.begin(), tags.end());
    }
}

size_t
PathNode::nTags() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return tags_.size();
}

std::vector<Tag::Ptr>
PathNode::tags() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return tags_;                                       // must be a copy for thread safety
}

Sawyer::Optional<rose_addr_t>
PathNode::address() const {
    // No lock necessary since the execution unit pointer cannot change after construction
    ASSERT_not_null(executionUnit_);
    return executionUnit_->address();
}

double
PathNode::processingTime() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return processingTime_;
}

void
PathNode::incrementProcessingTime(double seconds) {
    if (!rose_isnan(seconds)) {
        SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
        if (rose_isnan(processingTime_)) {
            processingTime_ = seconds;
        } else {
            processingTime_ += seconds;
        }
    }
}

std::string
PathNode::printableName() const {
    ASSERT_not_null(executionUnit_);
    return executionUnit_->printableName();
}

} // namespace
} // namespace
} // namespace

#endif

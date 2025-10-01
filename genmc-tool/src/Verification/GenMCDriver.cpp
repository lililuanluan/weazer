/*
 * GenMC -- Generic Model Checking.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-3.0.html.
 *
 * Author: Michalis Kokologiannakis <michalis@mpi-sws.org>
 */

#include "GenMCDriver.hpp"
#include "Config/Config.hpp"
#include "ExecutionGraph/DepExecutionGraph.hpp"
#include "ExecutionGraph/GraphIterators.hpp"
#include "ExecutionGraph/GraphUtils.hpp"
#include "ExecutionGraph/LabelVisitor.hpp"
#include "ExecutionGraph/MaximalIterator.hpp"
#include "Runtime/Interpreter.h"
#include "Static/LLVMModule.hpp"
#include "Support/Error.hpp"
#include "Support/Logger.hpp"
#include "Support/Parser.hpp"
#include "Support/SExprVisitor.hpp"
#include "Support/ThreadPool.hpp"
#include "Verification/Consistency/BoundDecider.hpp"
#include "Verification/Consistency/ConsistencyChecker.hpp"
#include "Verification/DriverHandlerDispatcher.hpp"
#include "config.h"
#include <llvm/IR/Verifier.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_os_ostream.h>

#include <algorithm>
#include <csignal>
#include <fstream>

/************************************************************
 ** GENERIC MODEL CHECKING DRIVER
 ***********************************************************/

GenMCDriver::GenMCDriver(std::shared_ptr<const Config> conf, std::unique_ptr<llvm::Module> mod,
			 std::unique_ptr<ModuleInfo> modInfo, ThreadPool *pool /* = nullptr */,
			 Mode mode /* = VerificationMode{} */)
	: userConf(std::move(conf)), pool(pool), mode(mode)
{
	/* Set up the execution context */
	auto execGraph = userConf->isDepTrackingModel ? std::make_unique<DepExecutionGraph>()
						      : std::make_unique<ExecutionGraph>();
	execStack.emplace_back(std::move(execGraph), std::move(LocalQueueT()),
			       std::move(ChoiceMap()));

	consChecker = ConsistencyChecker::create(getConf()->model);
	auto hasBounder = userConf->bound.has_value();
	GENMC_DEBUG(hasBounder |= userConf->boundsHistogram;);
	if (hasBounder)
		bounder = BoundDecider::create(getConf()->boundType);

	/* Create an interpreter for the program's instructions */
	std::string buf;
	EE = llvm::Interpreter::create(std::move(mod), std::move(modInfo), this, getConf(),
				       getAddrAllocator(), &buf);

	/* Set up a random-number generator (for the scheduler) */
	std::random_device rd;
	auto seedVal = (!userConf->randomScheduleSeed.empty())
			       ? (MyRNG::result_type)stoull(userConf->randomScheduleSeed)
			       : rd();
	if (userConf->printRandomScheduleSeed) {
		PRINT(VerbosityLevel::Error) << "Seed: " << seedVal << "\n";
	}
	rng.seed(seedVal);
	estRng.seed(rd());

	/*
	 * Make sure we can resolve symbols in the program as well. We use 0
	 * as an argument in order to load the program, not a library. This
	 * is useful as it allows the executions of external functions in the
	 * user code.
	 */
	std::string ErrorStr;
	if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr, &ErrorStr)) {
		WARN("Could not resolve symbols in the program: " + ErrorStr);
	}
}

GenMCDriver::~GenMCDriver() = default;

GenMCDriver::Execution::Execution(std::unique_ptr<ExecutionGraph> g, LocalQueueT &&w, ChoiceMap &&m)
	: graph(std::move(g)), workqueue(std::move(w)), choices(std::move(m))
{}
GenMCDriver::Execution::~Execution() = default;

void repairRead(ExecutionGraph &g, ReadLabel *lab)
{
	auto *maxLab = g.co_max(lab->getAddr());
	lab->setRf(maxLab);
	lab->setAddedMax(true);
	lab->setIPRStatus(maxLab->getStamp() > lab->getStamp());
}

void repairDanglingReads(ExecutionGraph &g)
{
	for (auto i = 0U; i < g.getNumThreads(); i++) {
		auto *rLab = llvm::dyn_cast<ReadLabel>(g.getLastThreadLabel(i));
		if (!rLab)
			continue;
		if (!rLab->getRf()) {
			repairRead(g, rLab);
		}
	}
}

void GenMCDriver::Execution::restrictGraph(Stamp stamp)
{
	/* Restrict the graph (and relations). It can be the case that
	 * events with larger stamp remain in the graph (e.g.,
	 * BEGINs). Fix their stamps too. */
	auto &g = getGraph();
	g.cutToStamp(stamp);
	g.compressStampsAfter(stamp);
	repairDanglingReads(g);
}

void GenMCDriver::Execution::restrictWorklist(Stamp stamp)
{
	std::vector<Stamp> idxsToRemove;

	auto &workqueue = getWorkqueue();
	for (auto rit = workqueue.rbegin(); rit != workqueue.rend(); ++rit)
		if (rit->first > stamp && rit->second.empty())
			idxsToRemove.push_back(rit->first); // TODO: break out of loop?

	for (auto &i : idxsToRemove)
		workqueue.erase(i);
}

void GenMCDriver::Execution::restrictChoices(Stamp stamp)
{
	auto &choices = getChoiceMap();
	for (auto cit = choices.begin(); cit != choices.end();) {
		if (cit->first > stamp.get()) {
			cit = choices.erase(cit);
		} else {
			++cit;
		}
	}
}

void GenMCDriver::Execution::restrict(Stamp stamp)
{
	restrictGraph(stamp);
	restrictWorklist(stamp);
	restrictChoices(stamp);
}

void GenMCDriver::pushExecution(Execution &&e) { execStack.push_back(std::move(e)); }

bool GenMCDriver::popExecution()
{
	if (execStack.empty())
		return false;
	execStack.pop_back();
	return !execStack.empty();
}

GenMCDriver::State::State(std::unique_ptr<ExecutionGraph> g, ChoiceMap &&m, SAddrAllocator &&a,
			  llvm::BitVector &&fds, ValuePrefixT &&c, Event la)
	: graph(std::move(g)), choices(std::move(m)), alloctor(std::move(a)), fds(std::move(fds)),
	  cache(std::move(c)), lastAdded(la)
{}
GenMCDriver::State::~State() = default;

void GenMCDriver::initFromState(std::unique_ptr<State> s)
{
	execStack.clear();
	execStack.emplace_back(std::move(s->graph), LocalQueueT(), std::move(s->choices));
	alloctor = std::move(s->alloctor);
	fds = std::move(s->fds);
	seenPrefixes = std::move(s->cache);
	lastAdded = s->lastAdded;
}

std::unique_ptr<GenMCDriver::State> GenMCDriver::extractState()
{
	auto cache = std::move(seenPrefixes);
	seenPrefixes.clear();
	return std::make_unique<State>(getGraph().clone(), ChoiceMap(getChoiceMap()),
				       SAddrAllocator(alloctor), llvm::BitVector(fds),
				       std::move(cache), lastAdded);
}

/* Returns a fresh address to be used from the interpreter */
SAddr GenMCDriver::getFreshAddr(const MallocLabel *aLab)
{
	/* The arguments to getFreshAddr() need to be well-formed;
	 * make sure the alignment is positive and a power of 2 */
	auto alignment = aLab->getAlignment();
	BUG_ON(alignment <= 0 || (alignment & (alignment - 1)) != 0);
	switch (aLab->getStorageDuration()) {
	case StorageDuration::SD_Automatic:
		return getAddrAllocator().allocAutomatic(
			aLab->getAllocSize(), alignment,
			aLab->getStorageType() == StorageType::ST_Durable,
			aLab->getAddressSpace() == AddressSpace::AS_Internal);
	case StorageDuration::SD_Heap:
		return getAddrAllocator().allocHeap(
			aLab->getAllocSize(), alignment,
			aLab->getStorageType() == StorageType::ST_Durable,
			aLab->getAddressSpace() == AddressSpace::AS_Internal);
	case StorageDuration::SD_Static: /* Cannot ask for fresh static addresses */
	default:
		BUG();
	}
	BUG();
	return SAddr();
}

int GenMCDriver::getFreshFd()
{
	int fd = fds.find_first_unset();

	/* If no available descriptor found, grow fds and try again */
	if (fd == -1) {
		fds.resize(2 * fds.size() + 1);
		return getFreshFd();
	}

	/* Otherwise, mark the file descriptor as used */
	markFdAsUsed(fd);
	return fd;
}

void GenMCDriver::markFdAsUsed(int fd)
{
	if (fd > fds.size())
		fds.resize(fd);
	fds.set(fd);
}

void GenMCDriver::resetThreadPrioritization() { threadPrios.clear(); }

bool GenMCDriver::isSchedulable(int thread) const
{
	auto &thr = getEE()->getThrById(thread);
	auto *lab = getGraph().getLastThreadLabel(thread);
	return !thr.ECStack.empty() && !llvm::isa<TerminatorLabel>(lab);
}

bool GenMCDriver::schedulePrioritized()
{
	/* Return false if no thread is prioritized */
	if (threadPrios.empty())
		return false;

	BUG_ON(getConf()->bound.has_value());

	const auto &g = getGraph();
	auto *EE = getEE();
	for (auto &e : threadPrios) {
		/* Skip unschedulable threads */
		if (!isSchedulable(e.thread))
			continue;

		/* Found a not-yet-complete thread; schedule it */
		EE->scheduleThread(e.thread);
		return true;
	}
	return false;
}

bool GenMCDriver::scheduleNextLTR()
{
	auto &g = getGraph();
	auto *EE = getEE();

	for (auto i = 0U; i < g.getNumThreads(); i++) {
		if (!isSchedulable(i))
			continue;

		/* Found a not-yet-complete thread; schedule it */
		EE->scheduleThread(i);
		return true;
	}

	/* No schedulable thread found */
	return false;
}

bool GenMCDriver::isNextThreadInstLoad(int tid)
{
	auto &I = getEE()->getThrById(tid).ECStack.back().CurInst;

	/* Overapproximate with function calls some of which might be modeled as loads */
	auto *ci = llvm::dyn_cast<llvm::CallInst>(I);
	return llvm::isa<llvm::LoadInst>(I) || llvm::isa<llvm::AtomicCmpXchgInst>(I) ||
	       llvm::isa<llvm::AtomicRMWInst>(I) ||
	       (ci && ci->getCalledFunction() &&
		hasGlobalLoadSemantics(ci->getCalledFunction()->getName().str()));
}

bool GenMCDriver::scheduleNextWF()
{
	auto &g = getGraph();
	auto *EE = getEE();

	/* First, schedule based on the EG */
	for (auto i = 0u; i < g.getNumThreads(); i++) {
		if (!isSchedulable(i))
			continue;

		if (g.containsPos(Event(i, EE->getThrById(i).globalInstructions + 1))) {
			EE->scheduleThread(i);
			return true;
		}
	}

	/* Try and find a thread that satisfies the policy.
	 * Keep an LTR fallback option in case this fails */
	long fallback = -1;
	for (auto i = 0u; i < g.getNumThreads(); i++) {
		if (!isSchedulable(i))
			continue;

		if (fallback == -1)
			fallback = i;
		if (!isNextThreadInstLoad(i)) {
			EE->scheduleThread(getFirstSchedulableSymmetric(i));
			return true;
		}
	}

	/* Otherwise, try to schedule the fallback thread */
	if (fallback != -1) {
		EE->scheduleThread(getFirstSchedulableSymmetric(fallback));
		return true;
	}
	return false;
}

int GenMCDriver::getFirstSchedulableSymmetric(int tid)
{
	if (!getConf()->symmetryReduction)
		return tid;

	auto firstSched = tid;
	auto symm = getSymmPredTid(tid);
	while (symm != -1) {
		if (isSchedulable(symm))
			firstSched = symm;
		symm = getSymmPredTid(symm);
	}
	return firstSched;
}

bool GenMCDriver::scheduleNextWFR()
{
	auto &g = getGraph();
	auto *EE = getEE();

	/* First, schedule based on the EG */
	for (auto i = 0u; i < g.getNumThreads(); i++) {
		if (!isSchedulable(i))
			continue;

		if (g.containsPos(Event(i, EE->getThrById(i).globalInstructions + 1))) {
			EE->scheduleThread(i);
			return true;
		}
	}

	std::vector<int> nonwrites;
	std::vector<int> writes;
	for (auto i = 0u; i < g.getNumThreads(); i++) {
		if (!isSchedulable(i))
			continue;

		if (!isNextThreadInstLoad(i)) {
			writes.push_back(i);
		} else {
			nonwrites.push_back(i);
		}
	}

	std::vector<int> &selection = !writes.empty() ? writes : nonwrites;
	if (selection.empty())
		return false;

	MyDist dist(0, selection.size() - 1);
	auto candidate = selection[dist(rng)];
	if (getConf()->interactiveAddGraph && selection.size() > 1) {
		fuzzPreviewCurGraph();
		llvm::dbgs() << "threads: " << format(selection) << '\n';
		int tid;
		do {
			llvm::dbgs() << ">>> ";
			std::cin >> tid;
		} while (std::ranges::find(selection, tid) == selection.end());
		candidate = tid;
	}
	EE->scheduleThread(getFirstSchedulableSymmetric(static_cast<int>(candidate)));
	return true;
}

bool GenMCDriver::scheduleNextRandom()
{
	auto &g = getGraph();
	auto *EE = getEE();

	/* Check if randomize scheduling is enabled and schedule some thread */
	MyDist dist(0, g.getNumThreads());
	auto random = dist(rng);
	for (auto j = 0u; j < g.getNumThreads(); j++) {
		auto i = (j + random) % g.getNumThreads();

		if (!isSchedulable(i))
			continue;

		/* Found a not-yet-complete thread; schedule it */
		EE->scheduleThread(getFirstSchedulableSymmetric(static_cast<int>(i)));
		return true;
	}

	/* No schedulable thread found */
	return false;
}

void GenMCDriver::resetExplorationOptions()
{
	unmoot();
	setRescheduledRead(Event::getInit());
	resetThreadPrioritization();
}

void GenMCDriver::handleExecutionStart()
{
	const auto &g = getGraph();

	/* Set-up (optimize) the interpreter for the new exploration */
	for (auto i = 1u; i < g.getNumThreads(); i++) {

		/* Skip not-yet-created threads */
		BUG_ON(g.isThreadEmpty(i));

		auto *labFst = g.getFirstThreadLabel(i);
		auto parent = labFst->getParentCreate();

		/* Skip if parent create does not exist yet (or anymore) */
		if (!g.containsPos(parent) ||
		    !llvm::isa<ThreadCreateLabel>(g.getEventLabel(parent)))
			continue;

		/* Skip finished threads */
		auto *labLast = g.getLastThreadLabel(i);
		if (llvm::isa<ThreadFinishLabel>(labLast))
			continue;

		/* Skip the recovery thread, if it exists.
		 * It will be scheduled separately afterwards */
		if (i == g.getRecoveryRoutineId())
			continue;

		/* Otherwise, initialize ECStacks in interpreter */
		auto &thr = getEE()->getThrById(i);
		BUG_ON(!thr.ECStack.empty());
		thr.ECStack = thr.initEC;
	}
}

std::pair<std::vector<SVal>, Event> GenMCDriver::extractValPrefix(Event pos)
{
	auto &g = getGraph();
	std::vector<SVal> vals;
	Event last;

	for (auto i = 0u; i < pos.index; i++) {
		auto *lab = g.getEventLabel(Event(pos.thread, i));
		if (auto *rLab = llvm::dyn_cast<ReadLabel>(lab)) {
			vals.push_back(getReadValue(rLab));
			last = lab->getPos();
		} else if (auto *jLab = llvm::dyn_cast<ThreadJoinLabel>(lab)) {
			vals.push_back(getJoinValue(jLab));
			last = lab->getPos();
		} else if (auto *bLab = llvm::dyn_cast<ThreadStartLabel>(lab)) {
			vals.push_back(getStartValue(bLab));
			last = lab->getPos();
		} else if (auto *oLab = llvm::dyn_cast<OptionalLabel>(lab)) {
			vals.push_back(SVal(oLab->isExpanded()));
			last = lab->getPos();
		} else {
			BUG_ON(lab->hasValue());
		}
	}
	return {vals, last};
}

Event findNextLabelToAdd(const ExecutionGraph &g, Event pos)
{
	const auto *firstLab = g.getFirstThreadLabel(pos.thread);
	auto succs = po_succs(g, firstLab);
	auto it =
		std::ranges::find_if(succs, [&](auto &lab) { return llvm::isa<EmptyLabel>(&lab); });
	return it == succs.end() ? g.getLastThreadLabel(pos.thread)->getPos().next()
				 : (*it).getPos();
}

bool GenMCDriver::tryOptimizeScheduling(Event pos)
{
	if (!getConf()->instructionCaching || !inVerificationMode())
		return false;

	auto next = findNextLabelToAdd(getGraph(), pos);
	auto [vals, last] = extractValPrefix(next);
	auto *res = retrieveCachedSuccessors(pos.thread, vals);
	if (res == nullptr || res->empty() || res->back()->getIndex() < next.index)
		return false;

	for (auto &vlab : *res) {
		BUG_ON(vlab->hasStamp());

		DriverHandlerDispatcher dispatcher(this);
		dispatcher.visit(vlab);
		if (llvm::isa<BlockLabel>(getGraph().getLastThreadLabel(vlab->getThread())) ||
		    isMoot() || getEE()->getCurThr().isBlocked() || isHalting())
			return true;
	}
	return true;
}

void GenMCDriver::checkHelpingCasAnnotation()
{
	/* If we were waiting for a helped CAS that did not appear, complain */
	auto &g = getGraph();
	for (auto i = 0U; i < g.getNumThreads(); i++) {
		if (llvm::isa<HelpedCASBlockLabel>(g.getLastThreadLabel(i)))
			ERROR("Helped/Helping CAS annotation error! Does helped CAS always "
			      "execute?\n");
	}

	/* Next, we need to check whether there are any extraneous
	 * stores, not visible to the helped/helping CAS */
	for (auto &lab : g.labels() | std::views::filter([](auto &lab) {
				 return llvm::isa<HelpingCasLabel>(&lab);
			 })) {
		auto *hLab = llvm::dyn_cast<HelpingCasLabel>(&lab);

		/* Check that all stores that would make this helping
		 * CAS succeed are read by a helped CAS.
		 * We don't need to check the swap value of the helped CAS */
		if (std::any_of(g.co_begin(hLab->getAddr()), g.co_end(hLab->getAddr()),
				[&](auto &sLab) {
					return hLab->getExpected() == sLab.getVal() &&
					       std::none_of(
						       sLab.readers_begin(), sLab.readers_end(),
						       [&](auto &rLab) {
							       return llvm::isa<HelpedCasReadLabel>(
								       &rLab);
						       });
				}))
			ERROR("Helped/Helping CAS annotation error! "
			      "Unordered store to helping CAS location!\n");

		/* Special case for the initializer (as above) */
		if (hLab->getAddr().isStatic() &&
		    hLab->getExpected() == getEE()->getLocInitVal(hLab->getAccess())) {
			auto rsView = g.labels() | std::views::filter([hLab](auto &lab) {
					      auto *rLab = llvm::dyn_cast<ReadLabel>(&lab);
					      return rLab && rLab->getAddr() == hLab->getAddr();
				      });
			if (std::ranges::none_of(rsView, [&](auto &lab) {
				    return llvm::isa<HelpedCasReadLabel>(&lab);
			    }))
				ERROR("Helped/Helping CAS annotation error! "
				      "Unordered store to helping CAS location!\n");
		}
	}
	return;
}

#ifdef ENABLE_GENMC_DEBUG
void GenMCDriver::trackExecutionBound()
{
	auto bound = bounder->calculate(getGraph());
	result.exploredBounds.grow(bound);
	result.exploredBounds[bound]++;
}
#endif

bool GenMCDriver::isExecutionBlocked() const
{
	return std::any_of(
		getEE()->threads_begin(), getEE()->threads_end(), [this](const llvm::Thread &thr) {
			// FIXME: was thr.isBlocked()
			auto &g = getGraph();
			if (thr.id >= g.getNumThreads() || g.isThreadEmpty(thr.id)) // think rec
				return false;
			return llvm::isa<BlockLabel>(g.getLastThreadLabel(thr.id));
		});
}

void GenMCDriver::updateStSpaceEstimation()
{
	/* Calculate current sample */
	auto &choices = getChoiceMap();
	auto sample = std::accumulate(choices.begin(), choices.end(), 1.0L,
				      [](auto sum, auto &kv) { return sum *= kv.second.size(); });

	/* This is the (i+1)-th exploration */
	auto totalExplored = (long double)result.explored + result.exploredBlocked + 1L;

	/* As the estimation might stop dynamically, we can't just
	 * normalize over the max samples to avoid overflows. Instead,
	 * use Welford's online algorithm to calculate mean and
	 * variance. */
	auto prevM = result.estimationMean;
	auto prevV = result.estimationVariance;
	result.estimationMean += (sample - prevM) / totalExplored;
	result.estimationVariance +=
		(sample - prevM) / totalExplored * (sample - result.estimationMean) -
		prevV / totalExplored;
}

void GenMCDriver::updateSeenValues(const ExecutionGraph &g)
{
	auto tids = g.thr_ids();
	std::for_each(tids.begin(), tids.end(), [&](int tid) {
		auto [vals, _] = extractValPrefix({tid, (int)g.getThreadSize(tid)});
		for (int i = 1; i <= vals.size(); i++) {
			auto seq = vals | std::views::take(i);
			auto *data = seenValues[tid].lookup(seq);
			if (!data) {
				seenValues[tid].addSeq(seq, 1);
			} else {
				++(*data);
			}
		}
	});
}

size_t GenMCDriver::Result::getGraphFreq(GraphHashT ghash) const
{
	if (auto it = graphFreqComplete.find(ghash); it != graphFreqComplete.end()) {
		return it->second;
	}
	if (auto it = graphFreqBlock.find(ghash); it != graphFreqBlock.end()) {
		return it->second;
	}
	BUG();
}

void GenMCDriver::updateFuzzingStats(const ExecutionGraph &g)
{
	auto &stats = result.fuzzStats;
	if (!getConf()->dumpFuzzingCoveragePlotFile.empty()) {
		stats.coverage.push_back(result.totalExploredDistinct());
	}
	switch (getConf()->fuzzIsInteresting) {
	case FuzzIsInteresting::GraphFreq:
		++stats.freqSum;
		break;
	case FuzzIsInteresting::GraphFreqSqured:
		// x^2 - (x-1)^2 = 2x-1
		stats.freqSum += (2 * result.getGraphFreq(getLastHash()) - 1);
		break;
	case FuzzIsInteresting::ValueSeq:
		stats.freqSum += computeValSeqScore(g);
		break;
	}
}

void GenMCDriver::updateGraphFrequencies(const ExecutionGraph &g)
{

	auto hash = getLastHash();
	if (isExecutionBlocked()) {
		++(getResult().graphFreqBlock)[hash];
	} else {
		++(getResult().graphFreqComplete)[hash];
	}
}

void GenMCDriver::recordAndDumpHashCollisions()
{
	auto ghash = getLastHash();
	std::string str;
	llvm::raw_string_ostream out(str);
	out << getGraph() << '\n';
	getResult().hashCount[ghash].push_back(std::move(str));
	int cnt = 0;
	if (auto &hs = getResult().hashCount[ghash]; hs.size() > 1) {
		++getResult().collisionNum;
		llvm::outs() << "Collision: " << getResult().collisionNum << " / "
			     << getResult().totalExplored() << '\n';
		for (auto &g : hs) {
			cnt++;
			std::ofstream fs(std::to_string(ghash) + "-collision-" +
					 std::to_string(cnt));
			fs << g << '\n';
		}
	}
}

void GenMCDriver::updateStSpaceChoicesWeights(const ExecutionGraph &g, ChoiceMap &choices)
{

	auto getRfInfo = [&](const Event &r, const Event &s) {
		auto *rLab = g.getReadLabel(r);
		auto *sLab = g.getEventLabel(s);
		auto val = getWriteValue(sLab, rLab->getAccess());

		// will (r, s) lead to new value sequence?
		auto [vals, last] = extractValPrefix(r);
		vals.push_back(val);
		auto *data = seenValues[r.thread].lookup(vals);
		BUG_ON(!data);
		bool newVal = (*data == 0);

		// will (r, s) lead to blocked executions?
		using Evaluator = SExprEvaluator<ModuleID::ID>;
		bool noBlock = true;
		if (rLab->getAnnot() && !Evaluator().evaluate(rLab->getAnnot(), val))
			noBlock = false;

		// is (r, s) backward revisit?
		auto rStamp = rLab->getStamp().get();
		auto sStamp = sLab->getStamp().get();
		bool backRev = sStamp > sStamp;

		// is s co-max
		bool coMax = sLab == g.getInitLabel()
				     ? false
				     : (sLab == g.co_max(g.getWriteLabel(s)->getAddr()));

		// is s rf-max
		auto stores = getRfsApproximation(rLab);
		BUG_ON(stores.empty());
		bool rfMax = (s == stores.back());

		return std::tuple{newVal, noBlock, backRev, coMax, rfMax};
	};

	const auto conf = getConf();

	for (auto &lab : g.labels()) {
		if (!llvm::isa<ReadLabel>(lab))
			continue;

		std::vector<std::pair<Event, double>> updated;
		updated.reserve(choices[lab.getStamp()].size());

		for (auto &[s, _] : choices[lab.getStamp()]) {

			auto [newVal, noBlock, backRev, coMax, rfMax] = getRfInfo(lab.getPos(), s);

			double weight = 1;

			if (conf->prioNewVal)
				weight += newVal;

			if (conf->prioBackRev)
				weight += backRev;

			if (conf->prioStaleStore)
				weight += !(coMax | rfMax);

			if (getConf()->fuzzValueNoblock)
				weight *= noBlock;

			if (getConf()->fuzzFilterSeenVals)
				weight *= newVal;

			updated.emplace_back(s, weight);
		}

		choices[lab.getStamp()].clear();

		for (auto &u : updated)
			choices[lab.getStamp()].insert(u);
	}
}
void GenMCDriver::fuzzPreviewCurGraph()
{
	const auto &g = getGraph();
	auto f = getConf()->dotFile + std::to_string(result.totalExplored());

	EventLabel const *lab = nullptr;
	Stamp stamp = 0;
	for (auto tid : g.thr_ids()) {
		if (g.getLastThreadLabel(tid)->getStamp() > stamp) {
			lab = g.getLastThreadLabel(tid);
			stamp = lab->getStamp();
		}
	}
	dotPrintToFile(f, lab, nullptr);
	std::string cmd = "dot \"" + f + "\" -Tpdf -o \"" + f + ".pdf\"";
	std::system(cmd.c_str());
	if (getConf()->interactiveAddGraph)
		llvm::dbgs() << "============\n" << g << "============\n" << '\n';
}

void GenMCDriver::handleExecutionEnd()
{
	if (inFuzzingMode() && getConf()->interactiveAddGraph) {
		llvm::dbgs() << getGraph() << '\n';
	}
	if (getConf()->countDistinctExecs || inFuzzingMode()) {
		updateGraphFrequencies(getGraph());
	}

	if (getConf()->testHashCollision && inVerificationMode()) {
		recordAndDumpHashCollisions();
	}

	if (inFuzzingMode()) {
		updateSeenValues(getGraph());
		updateFuzzingStats(getGraph());
		updateStSpaceChoicesWeights(getGraph(), getChoiceMap());
		if (!getConf()->dotFile.empty()) {
			fuzzPreviewCurGraph();
		}
	}

	if (isMoot()) {
		GENMC_DEBUG(++result.exploredMoot;);
		return;
	}

	/* Helper: Check helping CAS annotation */
	if (getConf()->helper)
		checkHelpingCasAnnotation();

	/* If under estimation mode, guess the total.
	 * (This may run a few times, but that's OK.)*/
	if (inEstimationMode()) {
		updateStSpaceEstimation();
		if (!shouldStopEstimating())
			addToWorklist(0, std::make_unique<RerunForwardRevisit>());
	}

	/* Ignore the execution if some assume has failed */
	if (isExecutionBlocked()) {
		++result.exploredBlocked;
		if (getConf()->printBlockedExecs)
			printGraph();
		if (getConf()->checkLiveness)
			checkLiveness();
		return;
	}

	if (getConf()->warnUnfreedMemory)
		checkUnfreedMemory();
	if (getConf()->printExecGraphs && !getConf()->persevere)
		printGraph(); /* Delay printing if persevere is enabled */

	GENMC_DEBUG(if (getConf()->boundsHistogram && inVerificationMode()) trackExecutionBound(););

	++result.explored;
	if (fullExecutionExceedsBound())
		++result.boundExceeding;
}

void GenMCDriver::handleRecoveryStart()
{
	BUG();
	// if (isExecutionBlocked())
	// 	return;

	// auto &g = getGraph();
	// auto *EE = getEE();

	// /* Make sure that a thread for the recovery routine is
	//  * added only once in the execution graph*/
	// if (g.getRecoveryRoutineId() == -1)
	// 	g.addRecoveryThread();

	// /* We will create a start label for the recovery thread.
	//  * We synchronize with a persistency barrier, if one exists,
	//  * otherwise, we synchronize with nothing */
	// auto tid = g.getRecoveryRoutineId();
	// auto psb = g.collectAllEvents(
	// 	[&](const EventLabel *lab) { return llvm::isa<DskPbarrierLabel>(lab); });
	// if (psb.empty())
	// 	psb.push_back(Event::getInit());
	// ERROR_ON(psb.size() > 1, "Usage of only one persistency barrier is allowed!\n");

	// auto tsLab = ThreadStartLabel::create(Event(tid, 0), psb.back(),
	// 				      ThreadInfo(tid, psb.back().thread, 0, 0));
	// auto *lab = addLabelToGraph(std::move(tsLab));

	// /* Create a thread for the interpreter, and appropriately
	//  * add it to the thread list (pthread_create() style) */
	// EE->createAddRecoveryThread(tid);

	// /* Finally, do all necessary preparations in the interpreter */
	// getEE()->setupRecoveryRoutine(tid);
	return;
}

void GenMCDriver::handleRecoveryEnd()
{
	/* Print the graph with the recovery routine */
	if (getConf()->printExecGraphs)
		printGraph();
	getEE()->cleanupRecoveryRoutine(getGraph().getRecoveryRoutineId());
	return;
}

void GenMCDriver::run()
{
	/* Explore all graphs and print the results */
	explore();
}

bool GenMCDriver::isHalting() const
{
	auto *tp = getThreadPool();
	return shouldHalt || (tp && tp->shouldHalt());
}

void GenMCDriver::halt(VerificationError status)
{
	shouldHalt = true;
	result.status = status;
	if (getThreadPool())
		getThreadPool()->halt();
}

GenMCDriver::Result GenMCDriver::verify(std::shared_ptr<const Config> conf,
					std::unique_ptr<llvm::Module> mod,
					std::unique_ptr<ModuleInfo> modInfo)
{
	/* Spawn a single or multiple drivers depending on the configuration */
	if (conf->threads == 1) {
		auto driver =
			GenMCDriver::create(conf, std::move(mod), std::move(modInfo), nullptr,
					    conf->fuzz ? GenMCDriver::Mode{FuzzingMode{}}
						       : GenMCDriver::Mode{VerificationMode{}});
		driver->run();
		auto res = driver->getResult();
		driver.release();
		return res;
		// return driver->getResult();
	}

	std::vector<std::future<GenMCDriver::Result>> futures;
	{
		/* Then, fire up the drivers */
		ThreadPool pool(conf, mod, modInfo);
		futures = pool.waitForTasks();
	}

	GenMCDriver::Result res;
	for (auto &f : futures) {
		res += f.get();
	}
	return res;
}

GenMCDriver::Result GenMCDriver::estimate(std::shared_ptr<const Config> conf,
					  const std::unique_ptr<llvm::Module> &mod,
					  const std::unique_ptr<ModuleInfo> &modInfo)
{
	auto estCtx = std::make_unique<llvm::LLVMContext>();
	auto newmod = LLVMModule::cloneModule(mod, estCtx);
	auto newMI = modInfo->clone(*newmod);
	auto driver = GenMCDriver::create(conf, std::move(newmod), std::move(newMI), nullptr,
					  GenMCDriver::EstimationMode{conf->estimationMax});
	driver->run();
	return driver->getResult();
}

void GenMCDriver::addToWorklist(Stamp stamp, WorkSet::ItemT item)
{
	getWorkqueue()[stamp].add(std::move(item));
}

std::pair<Stamp, WorkSet::ItemT> GenMCDriver::getNextItem()
{
	auto &workqueue = getWorkqueue();
	for (auto rit = workqueue.rbegin(); rit != workqueue.rend(); ++rit) {
		if (rit->second.empty()) {
			continue;
		}

		return {rit->first, rit->second.getNext()};
	}
	return {0, nullptr};
}

/************************************************************
 ** Scheduling methods
 ***********************************************************/

void GenMCDriver::blockThread(std::unique_ptr<BlockLabel> bLab)
{
	/* There are a couple of reasons we don't call Driver::addLabelToGraph() here:
	 *   1) It's redundant to update the views of the block label
	 *   2) If addLabelToGraph() does extra stuff (e.g., event caching) we absolutely
	 *      don't want to do that here. blockThread() should be safe to call from
	 *      anywhere in the code, with no unexpected side-effects */
	auto &g = getGraph();
	if (bLab->getPos() == g.getLastThreadLabel(bLab->getThread())->getPos())
		g.removeLast(bLab->getThread());
	g.addLabelToGraph(std::move(bLab));
}

void GenMCDriver::blockThreadTryMoot(std::unique_ptr<BlockLabel> bLab)
{
	auto pos = bLab->getPos();
	blockThread(std::move(bLab));
	mootExecutionIfFullyBlocked(pos);
}

void GenMCDriver::unblockThread(Event pos)
{
	auto *bLab = getGraph().getLastThreadLabel(pos.thread);
	BUG_ON(!llvm::isa<BlockLabel>(bLab));
	getGraph().removeLast(pos.thread);
}

bool GenMCDriver::scheduleAtomicity()
{
	auto *lastLab = getGraph().getEventLabel(lastAdded);
	if (llvm::isa<FaiReadLabel>(lastLab)) {
		getEE()->scheduleThread(lastAdded.thread);
		return true;
	}
	if (auto *casLab = llvm::dyn_cast<CasReadLabel>(lastLab)) {
		if (getReadValue(casLab) == casLab->getExpected()) {
			getEE()->scheduleThread(lastAdded.thread);
			return true;
		}
	}
	return false;
}

bool GenMCDriver::scheduleNormal()
{
	if (!inVerificationMode())
		return scheduleNextWFR();

	switch (getConf()->schedulePolicy) {
	case SchedulePolicy::ltr:
		return scheduleNextLTR();
	case SchedulePolicy::wf:
		return scheduleNextWF();
	case SchedulePolicy::wfr:
		return scheduleNextWFR();
	case SchedulePolicy::arbitrary:
		return scheduleNextRandom();
	default:
		BUG();
	}
	BUG();
}

bool GenMCDriver::rescheduleReads()
{
	auto &g = getGraph();
	auto *EE = getEE();

	for (auto i = 0u; i < g.getNumThreads(); ++i) {
		auto *bLab = llvm::dyn_cast<ReadOptBlockLabel>(g.getLastThreadLabel(i));
		if (!bLab)
			continue;

		BUG_ON(getConf()->bound.has_value());
		setRescheduledRead(bLab->getPos());
		unblockThread(bLab->getPos());
		EE->scheduleThread(i);
		return true;
	}
	return false;
}

bool GenMCDriver::scheduleNext()
{
	if (isMoot() || isHalting())
		return false;

	auto &g = getGraph();
	auto *EE = getEE();

	/* 1. Ensure atomicity. This needs to here because of weird interactions with in-place
	 * revisiting and thread priotitization. For example, consider the following scenario:
	 *     - restore @ T2, in-place rev @ T1, prioritize rev @ T1,
	 *       restore FAIR @ T2, schedule T1, atomicity violation */
	if (scheduleAtomicity()) {
		if (getConf()->interactiveAddGraph)
			llvm::dbgs() << "scheduled atomicity\n";
		return true;
	}

	/* Check if we should prioritize some thread */
	if (schedulePrioritized()) {
		if (getConf()->interactiveAddGraph)
			llvm::dbgs() << "scheduled prioritized\n";
		return true;
	}

	/* Schedule the next thread according to the chosen policy */
	if (scheduleNormal()) {
		if (getConf()->interactiveAddGraph)
			llvm::dbgs() << "scheduled normal\n";
		return true;
	}

	/* Finally, check if any reads needs to be rescheduled */
	return rescheduleReads();
}

std::vector<ThreadInfo> createExecutionContext(const ExecutionGraph &g)
{
	std::vector<ThreadInfo> tis;
	for (auto i = 1u; i < g.getNumThreads(); i++) { // skip main
		auto *bLab = g.getFirstThreadLabel(i);
		BUG_ON(!bLab);
		tis.push_back(bLab->getThreadInfo());
	}
	return tis;
}

std::pair<std::unique_ptr<ExecutionGraph>, std::unique_ptr<Revisit>>
GenMCDriver::mutate(const ExecutionGraph &g, const Event &r, const Event &w, const RevisitCut) const
{
	auto *rLab = g.getReadLabel(r);
	auto *wLab = g.getEventLabel(w);
	std::unique_ptr<VectorClock> v;
	std::unique_ptr<Revisit> rev;
	if (rLab->getStamp() > wLab->getStamp()) {
		v = g.getViewFromStamp(rLab->getStamp());
		rev = std::make_unique<ReadForwardRevisit>(r, w, true);

	} else {
		v = getRevisitView(rLab, llvm::dyn_cast<WriteLabel>(wLab));
		rev = constructBackwardRevisit(rLab, llvm::dyn_cast<WriteLabel>(wLab));
	}

	auto og = g.getCopyUpTo(*v);
	og->compressStampsAfter(rLab->getStamp());
	return {std::move(og), std::move(rev)};
}

auto GenMCDriver::calcMutationOptions(const ExecutionGraph &g, std::optional<unsigned int> bound)
	-> std::vector<
		std::tuple<std::unique_ptr<ExecutionGraph>, ChoiceMap, std::unique_ptr<Revisit>>>
{

	if (getConf()->mutation == MutationPolicy::NoMutation)
		return {};

	/* Collect all options that will lead to a different graph */
	using RevT = std::tuple<Event, Event, double>;
	std::vector<RevT> revs;
	for (auto &lab : g.labels()) {
		if (!llvm::isa<ReadLabel>(lab))
			continue;
		for (auto &[s, w] : getChoiceMap()[lab.getStamp()])
			if (s != llvm::dyn_cast<ReadLabel>(&lab)->getRf()->getPos()) {
				RevT rev = {lab.getPos(), s, w};
				revs.push_back(std::move(rev));
			}
	}

	auto mutation = getConf()->mutation;
	BUG_ON(!mutTable.contains(mutation));

	std::vector<
		std::tuple<std::unique_ptr<ExecutionGraph>, ChoiceMap, std::unique_ptr<Revisit>>>
		todos;
	auto populateTodos = [&](const ExecutionGraph &g, const Event &r, const Event &w) {
		auto [og, rev] = mutate(g, r, w, mutTable.at(mutation));
		auto m = createChoiceMapForCopy(*og);
		todos.emplace_back(std::move(og), std::move(m), std::move(rev));
	};

	auto getWeight = [&](auto &rev) -> double { return std::get<2>(rev); };

	// BUG_ON(!bound); // suppose we always need this bound. TODO: remove optional

	auto n = bound ? std::min<size_t>(*bound, revs.size()) : revs.size();

	std::vector<double> weights;
	std::ranges::transform(revs, std::back_inserter(weights), getWeight);

	while (n--) {
		if (std::ranges::all_of(weights, [](double w) { return w == 0; }))
			break;
		std::discrete_distribution<size_t> dist(weights.begin(), weights.end());
		auto idx = dist(rng);
		weights[idx] = 0;
		auto &[r, s, w] = revs[idx];
		populateTodos(g, r, s);
	}

	return todos;
}

bool GenMCDriver::isInteresting(const ExecutionGraph &g)
{
	const auto freq = result.getGraphFreq(getLastHash()) - 1;
	const auto &thres = getConf()->interestingThreshold;
	const auto &freqSum = result.fuzzStats.freqSum;

	std::uniform_real_distribution<double> dist(0, 1);

	switch (getConf()->fuzzIsInteresting) {
	case FuzzIsInteresting::Always:
		return true;
	case FuzzIsInteresting::NewGraphs:
		return freq == 0;
	case FuzzIsInteresting::GraphFreq:
		BUG_ON(freqSum == 0);
		return (((float)freq * result.totalExploredDistinct()) / result.totalExplored()) <=
		       thres;
	case FuzzIsInteresting::GraphFreqSqured:
		BUG_ON(freqSum == 0);
		return ((float)freq * freq * result.totalExploredDistinct()) / freqSum <= thres;
	case FuzzIsInteresting::Random:
		return (dist(rng) <= thres);
	case FuzzIsInteresting::ValueSeq:
		return computeValSeqScore(g) * result.totalExplored() / freqSum <= thres;
	default:
		BUG();
	}
}

float GenMCDriver::computeValSeqScore(const ExecutionGraph &g, int tid)
{
	auto [vals, _] = extractValPrefix({tid, (int)g.getThreadSize(tid)});

	unsigned int prev{0}, cur{0}, depth{0};
	float score = 0;
	for (int i = 1; i < vals.size(); i++) {
		if (auto *data = seenValues[tid].lookup(vals | std::views::take(i))) {
			cur = *data;
			if (prev > cur) {
				float gain = (float)cur / (prev * std::pow(2, depth++));
				score += gain;
			}
			prev = cur;
		}
	}

	return score;
}

float GenMCDriver::computeValSeqScore(const ExecutionGraph &g)
{
	auto tids = g.thr_ids();
	auto total = std::transform_reduce(tids.begin(), tids.end(), .0f, std::plus<>(),
					   [&](int tid) { return computeValSeqScore(g, tid); });
	return total / tids.size();
}

void GenMCDriver::cacheCurrentExecution()
{
	const auto &g = getGraph();
	if (auto cpSize = getConf()->fuzzCorpusSize; cpSize > 0) {
		auto og = g.clone();
		auto m = createChoiceMapForCopy(*og);
		fuzzSeeds.push_back({std::move(og), LocalQueueT(), std::move(m)});

		if (fuzzSeeds.size() > cpSize) {
			MyDist dist(0, fuzzSeeds.size() - 1);
			auto it = fuzzSeeds.begin();
			std::advance(it, dist(rng));
			fuzzSeeds.erase(it); // randomly erase one seed
		}
	}
}

GenMCDriver::Execution GenMCDriver::fuzzPickNextGraph()
{
	BUG_ON(fuzzSeeds.size() == 0);
	// randomly pick
	MyDist dist(0, fuzzSeeds.size() - 1);
	auto it = fuzzSeeds.begin();
	std::advance(it, dist(rng));

	auto og = it->getGraph().clone(); // TODO: fix this extra copy
	auto m = it->getChoiceMap();
	fuzzSeeds.erase(it);
	return {std::move(og), LocalQueueT{}, std::move(m)};
}

void GenMCDriver::rotateExecStack(unsigned n)
{
	// an alternative way would be replace ExecStack with deque and popback + pushfront
	std::ranges::rotate(execStack, std::prev(execStack.end(), n));
}

void GenMCDriver::setEmptyGraphFlag()
{
	const auto &g = getGraph();
	lastGEmpty = true;
	lastGStamp = 0;
	for (auto t : getGraph().thr_ids()) {
		lastGEmpty &= (g.getThreadSize(t) <= 1);
		for (int i = 0; i < g.getThreadSize(t); i++) {
			lastGStamp = std::max(lastGStamp, g.getEventLabel({t, i})->getStamp());
		}
	}
}

void GenMCDriver::explore()
{
	auto *EE = getEE();

	resetExplorationOptions();
	EE->setExecutionContext(createExecutionContext(getGraph()));

	DumpGuard dg{this};

	while (!isHalting()) {
		EE->reset();

		clearLastHash();
		setEmptyGraphFlag();

		// llvm::dbgs() << "running with: " << getGraph() << '\n';

		/* Get main program function and run the program */
		EE->runAsMain(getConf()->programEntryFun);
		if (getConf()->persevere)
			EE->runRecovery();

		dg.tick();

		auto &fz = result.fuzzStats;
		if (inFuzzingMode()) {

			auto &g = getGraph();

			if (shouldStopFuzzing()) {
				return;
			}

			bool interesting = isInteresting(g);
			if (interesting ||
			    getResult().totalExplored() < getConf()->fuzzCorpusSize) {
				cacheCurrentExecution();
			}

			if (interesting) {
				++fz.interestingCount;
			}

			if (shouldSkipFuzzing()) {
				addToWorklist(0, std::make_unique<RerunForwardRevisit>());
				++fz.randCount;
			} else if (fuzzSeeds.size() > 0) {
				auto todos = [&]() {
					bool pushed = false;
					if (getConf()->fuzzCorpusSize > 0) {
						pushExecution(fuzzPickNextGraph());
						pushed = true;
					}
					auto todos = calcMutationOptions(getGraph(),
									 getConf()->mutationBound);
					if (pushed) {
						popExecution();
					}
					return todos;
				}();

				if (auto n = getConf()->mutationBound) {
					BUG_ON(todos.size() > *n);
				}

				fz.mutationCount += todos.size();

				// push found options
				for (int idx = 0; auto &[og, m, rev] : todos) {
					// TODO: this is a temporary fix of that: when a previours
					// revisit was handled, atomicity violation happened and
					// moot() was called. but somehow unmoot wasn't called so it
					// affects the current revisit, which doesn't violate
					// atomicity
					unmoot();
					pushExecution({std::move(og), LocalQueueT(), std::move(m)});
					repairDanglingReads(getGraph());
					auto ok = revisitRead(*rev);

					if (auto i = idx++;
					    (i == 0 && !getConf()->useQueue) ||
					    (i == todos.size() - 1 && getConf()->useQueue)) {
						addToWorklist(
							0, std::make_unique<RerunForwardRevisit>());
						++fz.randCount;
					}

					auto *rLab = getGraph().getEventLabel(rev->getPos());
					auto *wLab = getGraph().getWriteLabel(
						llvm::dyn_cast<ReadRevisit>(&*rev)->getRev());
					if (ok) {
						auto stamp = std::max(rLab->getStamp(),
								      wLab ? wLab->getStamp()
									   : Stamp{0});
						addToWorklist(
							stamp,
							std::make_unique<RerunForwardRevisit>());
						if (getConf()->useQueue) {
							rotateExecStack();
						}
						continue;
					}

					// there might be an rmw atomicity violation
					auto &g = getGraph();
					auto w = rev->getPos().next(); // such violations are
					// created by read frevs

					auto *nLab = g.getWriteLabel(w);
					BUG_ON(!g.violatesAtomicity(nLab));

					// revisit to ensure consistency
					auto rconf = g.getPendingRMW(nLab);
					BUG_ON(rconf.isInitializer());

					auto br = constructBackwardRevisit(g.getReadLabel(rconf),
									   nLab);
					auto v = getRevisitView(g.getReadLabel(rconf), nLab);

					auto newg = copyGraph(&*br, &*v);
					auto newm = createChoiceMapForCopy(*newg);
					auto newq = std::move(getWorkqueue());

					popExecution();
					pushExecution({std::move(newg), std::move(newq),
						       std::move(newm)});

					unmoot();
					repairDanglingReads(getGraph());
					ok = revisitRead(*br);
					BUG_ON(!ok);
					if (getConf()->useQueue) {
						rotateExecStack();
					}
				}
			}
		}

		auto validExecution = false;
		while (!validExecution) {
			/*
			 * restrictAndRevisit() might deem some execution infeasible,
			 * so we have to reset all exploration options before
			 * calling it again
			 */
			resetExplorationOptions();
			auto [stamp, item] = getNextItem();
			if (!item) {
				if (popExecution())
					continue;
				if (inFuzzingMode() && !shouldStopFuzzing()) {
					auto execGraph =
						userConf->isDepTrackingModel
							? std::make_unique<DepExecutionGraph>()
							: std::make_unique<ExecutionGraph>();
					execStack.emplace_back(std::move(execGraph),
							       std::move(LocalQueueT()),
							       std::move(ChoiceMap()));
					addToWorklist(0, std::make_unique<RerunForwardRevisit>());
					++fz.randCount;
					getEE()->resetClear();
					continue;
				}
				return;
			}
			auto pos = item->getPos();
			validExecution = restrictAndRevisit(stamp, item) && isRevisitValid(*item);
		}
	}
}

bool isUninitializedAccess(const SAddr &addr, const Event &pos)
{
	return addr.isDynamic() && pos.isInitializer();
}

bool GenMCDriver::isExecutionValid(const EventLabel *lab)
{

	return isSymmetryOK(lab) && getConsChecker().isConsistent(lab) &&
	       !partialExecutionExceedsBound();
}

bool GenMCDriver::isRevisitValid(const Revisit &revisit)
{
	auto &g = getGraph();
	auto pos = revisit.getPos();
	auto *mLab = llvm::dyn_cast<MemAccessLabel>(g.getEventLabel(pos));

	/* E.g., for optional revisits, do nothing */
	if (!mLab)
		return true;

	if (!isExecutionValid(mLab))
		return false;

	auto *rLab = llvm::dyn_cast<ReadLabel>(mLab);
	if (rLab && checkInitializedMem(rLab) != VerificationError::VE_OK)
		return false;

	/* If an extra event is added, re-check consistency */
	auto *nLab = g.getNextLabel(mLab);
	return !rLab || !rLab->isRMW() ||
	       (isExecutionValid(nLab) && checkForRaces(nLab) == VerificationError::VE_OK);
}

bool GenMCDriver::isExecutionDrivenByGraph(const EventLabel *lab)
{
	const auto &g = getGraph();
	auto curr = lab->getPos();
	return (curr.index < g.getThreadSize(curr.thread)) &&
	       !llvm::isa<EmptyLabel>(g.getEventLabel(curr));
}

bool GenMCDriver::executionExceedsBound(BoundCalculationStrategy strategy) const
{
	if (!getConf()->bound.has_value() || !inVerificationMode())
		return false;

	return bounder->doesExecutionExceedBound(getGraph(), *getConf()->bound, strategy);
}

bool GenMCDriver::fullExecutionExceedsBound() const
{
	return executionExceedsBound(BoundCalculationStrategy::NonSlacked);
}

bool GenMCDriver::partialExecutionExceedsBound() const
{
	return executionExceedsBound(BoundCalculationStrategy::Slacked);
}

bool GenMCDriver::inRecoveryMode() const
{
	return getEE()->getProgramState() == llvm::ProgramState::Recovery;
}

bool GenMCDriver::inReplay() const
{
	return getEE()->getExecState() == llvm::ExecutionState::Replay;
}

EventLabel *GenMCDriver::addLabelToGraph(std::unique_ptr<EventLabel> lab)
{
	auto &g = getGraph();

	/* Cache the event before updating views (inits are added w/ tcreate) */
	if (lab->getIndex() > 0)
		cacheEventLabel(&*lab);

	/* Add and update views */
	auto *addedLab = g.addLabelToGraph(std::move(lab));
	updateLabelViews(addedLab);
	if (auto *mLab = llvm::dyn_cast<MemAccessLabel>(addedLab))
		g.addAlloc(findAllocatingLabel(g, mLab->getAddr()), mLab);

	lastAdded = addedLab->getPos();
	if (addedLab->getIndex() >= getConf()->warnOnGraphSize) {
		LOG_ONCE("large-graph", VerbosityLevel::Tip)
			<< "The execution graph seems quite large. "
			<< "Consider bounding all loops or using -unroll\n";
	}
	return addedLab;
}

void GenMCDriver::updateLabelViews(EventLabel *lab)
{
	getConsChecker().updateMMViews(lab);
	if (!getConf()->symmetryReduction)
		return;

	auto &v = lab->getPrefixView();
	updatePrefixWithSymmetriesSR(lab);
}

VerificationError GenMCDriver::checkForRaces(const EventLabel *lab)
{
	if (getConf()->disableRaceDetection || !inVerificationMode())
		return VerificationError::VE_OK;

	/* Bounding: extensibility not guaranteed; RD should be disabled */
	if (llvm::isa<WriteLabel>(lab) && !checkAtomicity(llvm::dyn_cast<WriteLabel>(lab))) {
		BUG_ON(!getConf()->bound.has_value());
		return VerificationError::VE_OK;
	}

	/* Check for hard errors */
	const EventLabel *racyLab = nullptr;
	auto err = getConsChecker().checkErrors(lab, racyLab);
	if (err != VerificationError::VE_OK) {
		reportError({lab->getPos(), err, "", racyLab});
		return err;
	}

	/* Check whether there are any unreported warnings... */
	std::vector<const EventLabel *> races;
	auto newWarnings = getConsChecker().checkWarnings(lab, getResult().warnings, races);

	/* ... and report them */
	auto i = 0U;
	for (auto &wcode : newWarnings) {
		if (reportWarningOnce(lab->getPos(), wcode, races[i++]))
			return wcode;
	}
	return VerificationError::VE_OK;
}

void GenMCDriver::cacheEventLabel(const EventLabel *lab)
{
	if (!getConf()->instructionCaching || !inVerificationMode())
		return;

	auto &g = getGraph();

	/* Extract value prefix and cached data */
	auto [vals, last] = extractValPrefix(lab->getPos());
	auto *data = retrieveCachedSuccessors(lab->getThread(), vals);

	/*
	 * Check if there are any new data to cache.
	 * (For dep-tracking, we could optimize toIdx and collect until
	 * a new (non-empty) label with a value is found.)
	 */
	auto fromIdx = (!data || data->empty()) ? last.index : data->back()->getIndex();
	auto toIdx = lab->getIndex();
	if (data && !data->empty() && data->back()->getIndex() >= toIdx)
		return;

	/*
	 * Go ahead and collect the new data. We have to be careful when
	 * cloning LAB because it has not been added to the graph yet.
	 */
	std::vector<std::unique_ptr<EventLabel>> labs;
	for (auto i = fromIdx + 1; i <= toIdx; i++) {
		auto cLab = (i == lab->getIndex())
				    ? lab->clone()
				    : g.getEventLabel(Event(lab->getThread(), i))->clone();
		cLab->reset();
		labs.push_back(std::move(cLab));
	}

	/* Is there an existing entry? */
	if (!data) {
		auto res = seenPrefixes[lab->getThread()].addSeq(vals, std::move(labs));
		BUG_ON(!res);
		return;
	}

	BUG_ON(data->empty() && last.index >= lab->getIndex());
	BUG_ON(!data->empty() && data->back()->getIndex() + 1 != lab->getIndex());

	data->reserve(data->size() + labs.size());
	std::move(std::begin(labs), std::end(labs), std::back_inserter(*data));
	labs.clear();
}

/* Given an event in the graph, returns the value of it */
SVal GenMCDriver::getWriteValue(const EventLabel *lab, const AAccess &access)
{
	/* If the even represents an invalid access, return some value */
	if (!lab)
		return SVal();

	/* If the event is the initializer, ask the interpreter about
	 * the initial value of that memory location */
	if (lab->getPos().isInitializer())
		return getEE()->getLocInitVal(access);

	/* Otherwise, we will get the value from the execution graph */
	auto *wLab = llvm::dyn_cast<WriteLabel>(lab);
	BUG_ON(!wLab);

	/* It can be the case that the load's type is different than
	 * the one the write's (see troep.c).  In any case though, the
	 * sizes should match */
	if (wLab->getSize() != access.getSize())
		reportError({wLab->getPos(), VerificationError::VE_MixedSize,
			     "Mixed-size accesses detected: tried to read event with a " +
				     std::to_string(access.getSize().get() * 8) + "-bit access!\n" +
				     "Please check the LLVM-IR.\n"});

	/* If the size of the R and the W are the same, we are done */
	return wLab->getVal();
}

/* Same as above, but the data of a file are not explicitly initialized
 * so as not to pollute the graph with events, since a file can be large.
 * Thus, we treat the case where WRITE reads INIT specially. */
SVal GenMCDriver::getDskWriteValue(const EventLabel *lab, const AAccess &access)
{
	if (lab->getPos().isInitializer())
		return SVal();
	return getWriteValue(lab, access);
}

SVal GenMCDriver::getJoinValue(const ThreadJoinLabel *jLab) const
{
	auto &g = getGraph();
	auto *lLab = llvm::dyn_cast<ThreadFinishLabel>(g.getLastThreadLabel(jLab->getChildId()));
	BUG_ON(!lLab);
	return lLab->getRetVal();
}

SVal GenMCDriver::getStartValue(const ThreadStartLabel *bLab) const
{
	auto &g = getGraph();
	if (bLab->getPos().isInitializer() || bLab->getThread() == g.getRecoveryRoutineId())
		return SVal();

	return bLab->getThreadInfo().arg;
}

SVal GenMCDriver::getBarrierInitValue(const AAccess &access)
{
	const auto &g = getGraph();
	auto sIt = std::find_if(g.co_begin(access.getAddr()), g.co_end(access.getAddr()),
				[&access, &g](auto &bLab) {
					BUG_ON(!llvm::isa<WriteLabel>(bLab));
					return bLab.getAddr() == access.getAddr() &&
					       bLab.isNotAtomic();
				});

	/* All errors pertinent to initialization should be captured elsewhere */
	BUG_ON(sIt == g.co_end(access.getAddr()));
	return getWriteValue(&*sIt, access);
}

std::optional<SVal> GenMCDriver::getReadRetValue(const ReadLabel *rLab)
{
	/* Bottom is an acceptable re-option only @ replay */
	if (!rLab->getRf()) {
		BUG_ON(!inReplay());
		return std::nullopt;
	}

	/* Reading a non-init barrier value means that the thread should block */
	auto res = getReadValue(rLab);
	BUG_ON(llvm::isa<BWaitReadLabel>(rLab) && res != getBarrierInitValue(rLab->getAccess()) &&
	       !llvm::isa<TerminatorLabel>(getGraph().getLastThreadLabel(rLab->getThread())));
	return {res};
}

SVal GenMCDriver::getRecReadRetValue(const ReadLabel *rLab)
{
	auto &g = getGraph();

	/* Find and read from the latest sameloc store */
	auto preds = po_preds(g, rLab);
	auto wLabIt = std::ranges::find_if(preds, [rLab](auto &lab) {
		auto *wLab = llvm::dyn_cast<WriteLabel>(&lab);
		return wLab && wLab->getAddr() == rLab->getAddr();
	});
	BUG_ON(wLabIt == std::ranges::end(preds));
	return getWriteValue(&*wLabIt, rLab->getAccess());
}

VerificationError GenMCDriver::checkAccessValidity(const MemAccessLabel *lab)
{
	/* Static variable validity is handled by the interpreter. *
	 * Dynamic accesses are valid if they access allocated memory */
	if ((!lab->getAddr().isDynamic() && !getEE()->isStaticallyAllocated(lab->getAddr())) ||
	    (lab->getAddr().isDynamic() && !lab->getAlloc())) {
		reportError({lab->getPos(), VerificationError::VE_AccessNonMalloc});
		return VerificationError::VE_AccessNonMalloc;
	}
	return VerificationError::VE_OK;
}

VerificationError GenMCDriver::checkInitializedMem(const ReadLabel *rLab)
{
	// FIXME: Have label for mutex-destroy and check type instead of val.
	//        Also for barriers.

	/* Locks should not read from destroyed mutexes */
	const auto *lLab = llvm::dyn_cast<LockCasReadLabel>(rLab);
	if (lLab && getWriteValue(lLab->getRf(), lLab->getAccess()) == SVal(-1)) {
		reportError({lLab->getPos(), VerificationError::VE_UninitializedMem,
			     "Called lock() on destroyed mutex!", lLab->getRf()});
		return VerificationError::VE_UninitializedMem;
	}

	/* Barriers should read initialized, not-destroyed memory */
	const auto *bLab = llvm::dyn_cast<BIncFaiReadLabel>(rLab);
	if (bLab && bLab->getRf()->getPos().isInitializer()) {
		reportError({rLab->getPos(), VerificationError::VE_UninitializedMem,
			     "Called barrier_wait() on uninitialized barrier!"});
		return VerificationError::VE_UninitializedMem;
	}
	if (bLab && getWriteValue(bLab->getRf(), bLab->getAccess()) == SVal(0)) {
		reportError({rLab->getPos(), VerificationError::VE_AccessFreed,
			     "Called barrier_wait() on destroyed barrier!", bLab->getRf()});
		return VerificationError::VE_UninitializedMem;
	}

	/* Plain events should read initialized memory if they are dynamic accesses */
	if (isUninitializedAccess(rLab->getAddr(), rLab->getRf()->getPos())) {
		reportError({rLab->getPos(), VerificationError::VE_UninitializedMem});
		return VerificationError::VE_UninitializedMem;
	}
	return VerificationError::VE_OK;
}

VerificationError GenMCDriver::checkInitializedMem(const WriteLabel *wLab)
{
	auto &g = getGraph();

	/* Unlocks should unlock mutexes locked by the same thread */
	const auto *uLab = llvm::dyn_cast<UnlockWriteLabel>(wLab);
	if (uLab && !findMatchingLock(uLab)) {
		reportError({uLab->getPos(), VerificationError::VE_InvalidUnlock,
			     "Called unlock() on mutex not locked by the same thread!"});
		return VerificationError::VE_InvalidUnlock;
	}

	/* Barriers should be initialized once, with a proper value */
	const auto *bLab = llvm::dyn_cast<BInitWriteLabel>(wLab);
	if (bLab && wLab->getVal() == SVal(0)) {
		reportError({wLab->getPos(), VerificationError::VE_InvalidBInit,
			     "Called barrier_init() with 0!"});
		return VerificationError::VE_InvalidBInit;
	}
	if (bLab &&
	    std::any_of(g.co_begin(bLab->getAddr()), g.co_end(bLab->getAddr()), [&](auto &sLab) {
		    return &sLab != wLab && sLab.getAddr() == wLab->getAddr() &&
			   llvm::isa<BInitWriteLabel>(sLab);
	    })) {
		reportError({wLab->getPos(), VerificationError::VE_InvalidBInit,
			     "Called barrier_init() multiple times!"});
		return VerificationError::VE_InvalidBInit;
	}
	return VerificationError::VE_OK;
}

VerificationError GenMCDriver::checkFinalAnnotations(const WriteLabel *wLab)
{
	if (!getConf()->helper)
		return VerificationError::VE_OK;

	auto &g = getGraph();

	if (g.hasLocMoreThanOneStore(wLab->getAddr()))
		return VerificationError::VE_OK;
	if ((wLab->isFinal() &&
	     std::any_of(g.co_begin(wLab->getAddr()), g.co_end(wLab->getAddr()),
			 [&](auto &sLab) {
				 return !getConsChecker().getHbView(wLab).contains(sLab.getPos());
			 })) ||
	    (!wLab->isFinal() && std::any_of(g.co_begin(wLab->getAddr()), g.co_end(wLab->getAddr()),
					     [&](auto &sLab) { return sLab.isFinal(); }))) {
		reportError({wLab->getPos(), VerificationError::VE_Annotation,
			     "Multiple stores at final location!"});
		return VerificationError::VE_Annotation;
	}
	return VerificationError::VE_OK;
}

VerificationError GenMCDriver::checkIPRValidity(const ReadLabel *rLab)
{
	if (!rLab->getAnnot() || !getConf()->ipr)
		return VerificationError::VE_OK;

	auto &g = getGraph();
	auto racyIt = std::find_if(g.co_begin(rLab->getAddr()), g.co_end(rLab->getAddr()),
				   [&](auto &wLab) { return wLab.hasAttr(WriteAttr::WWRacy); });
	if (racyIt == g.co_end(rLab->getAddr()))
		return VerificationError::VE_OK;

	auto msg = "Unordered writes do not constitute a bug per se, though they often "
		   "indicate faulty design.\n"
		   "This warning is treated as an error due to in-place revisiting (IPR).\n"
		   "You can use -disable-ipr to disable this feature."s;
	reportError({racyIt->getPos(), VerificationError::VE_WWRace, msg, nullptr, true});
	return VerificationError::VE_WWRace;
}

bool GenMCDriver::threadReadsMaximal(int tid)
{
	auto &g = getGraph();

	/*
	 * Depending on whether this is a DSA loop or not, we have to
	 * adjust the detection starting point: DSA-blocked threads
	 * will have a SpinStart as their last event.
	 */
	BUG_ON(!llvm::isa<BlockLabel>(g.getLastThreadLabel(tid)));
	auto *lastLab = g.getPreviousLabel(g.getLastThreadLabel(tid));
	auto start = llvm::isa<SpinStartLabel>(lastLab) ? lastLab->getPos().prev()
							: lastLab->getPos();
	for (auto j = start.index; j > 0; j--) {
		auto *lab = g.getEventLabel(Event(tid, j));
		BUG_ON(llvm::isa<LoopBeginLabel>(lab));
		if (llvm::isa<SpinStartLabel>(lab))
			return true;
		if (auto *rLab = llvm::dyn_cast<ReadLabel>(lab)) {
			if (rLab->getRf() != g.co_max(rLab->getAddr()))
				return false;
		}
	}
	BUG();
}

void GenMCDriver::checkLiveness()
{
	if (isHalting())
		return;

	const auto &g = getGraph();

	/* Collect all threads blocked at spinloops */
	std::vector<int> spinBlocked;
	for (auto i = 0U; i < g.getNumThreads(); i++) {
		if (llvm::isa<SpinloopBlockLabel>(g.getLastThreadLabel(i)))
			spinBlocked.push_back(i);
	}

	if (spinBlocked.empty())
		return;

	/* And check whether all of them are live or not */
	auto nonTermTID = 0u;
	if (std::all_of(spinBlocked.begin(), spinBlocked.end(), [&](int tid) {
		    nonTermTID = tid;
		    return threadReadsMaximal(tid);
	    })) {
		/* Print some TID blocked by a spinloop */
		reportError({g.getLastThreadLabel(nonTermTID)->getPos(),
			     VerificationError::VE_Liveness,
			     "Non-terminating spinloop: thread " + std::to_string(nonTermTID)});
	}
	return;
}

void GenMCDriver::checkUnfreedMemory()
{
	if (isHalting())
		return;

	auto &g = getGraph();
	const MallocLabel *unfreedAlloc = nullptr;
	if (std::ranges::any_of(g.labels(), [&](auto &lab) {
		    unfreedAlloc = llvm::dyn_cast<MallocLabel>(&lab);
		    return unfreedAlloc && unfreedAlloc->getFree() == nullptr;
	    })) {
		reportWarningOnce(unfreedAlloc->getPos(), VerificationError::VE_UnfreedMemory);
	}
}

void GenMCDriver::filterConflictingBarriers(const ReadLabel *lab, std::vector<Event> &stores)
{
	if (getConf()->disableBAM ||
	    (!llvm::isa<BIncFaiReadLabel>(lab) && !llvm::isa<BWaitReadLabel>(lab)))
		return;

	/* barrier_wait()'s plain load should read maximally */
	if (auto *rLab = llvm::dyn_cast<BWaitReadLabel>(lab)) {
		std::swap(stores[0], stores.back());
		stores.resize(1);
		return;
	}

	/* barrier_wait()'s FAI loads should not read from conflicting stores */
	auto &g = getGraph();
	auto isReadByExclusiveRead = [&](auto *oLab) {
		if (auto *wLab = llvm::dyn_cast<WriteLabel>(oLab))
			return std::ranges::any_of(wLab->readers(),
						   [&](auto &rLab) { return rLab.isRMW(); });
		if (auto *iLab = llvm::dyn_cast<InitLabel>(oLab))
			return std::ranges::any_of(iLab->rfs(lab->getAddr()),
						   [&](auto &rLab) { return rLab.isRMW(); });
		BUG();
	};
	stores.erase(
		std::remove_if(stores.begin(), stores.end(),
			       [&](auto &s) { return isReadByExclusiveRead(g.getEventLabel(s)); }),
		stores.end());
	return;
}

int GenMCDriver::getSymmPredTid(int tid) const
{
	auto &g = getGraph();
	return g.getFirstThreadLabel(tid)->getSymmetricTid();
}

int GenMCDriver::getSymmSuccTid(int tid) const
{
	auto &g = getGraph();
	auto symm = tid;

	/* Check if there is anyone else symmetric to SYMM */
	for (auto i = tid + 1; i < g.getNumThreads(); i++)
		if (g.getFirstThreadLabel(i)->getSymmetricTid() == symm)
			return i;
	return -1; /* no one else */
}

bool GenMCDriver::isEcoBefore(const EventLabel *lab, int tid) const
{
	auto &g = getGraph();
	if (!llvm::isa<MemAccessLabel>(lab))
		return false;

	auto symmPos = Event(tid, lab->getIndex());
	// if (auto *wLab = rf_pred(g, lab); wLab) {
	// 	return wLab.getPos() == symmPos;
	// }))
	// 	return true;
	if (std::any_of(co_succ_begin(g, lab), co_succ_end(g, lab), [&](auto &sLab) {
		    return sLab.getPos() == symmPos ||
			   std::any_of(sLab.readers_begin(), sLab.readers_end(),
				       [&](auto &rLab) { return rLab.getPos() == symmPos; });
	    }))
		return true;
	if (std::any_of(fr_succ_begin(g, lab), fr_succ_end(g, lab), [&](auto &sLab) {
		    return sLab.getPos() == symmPos ||
			   std::any_of(sLab.readers_begin(), sLab.readers_end(),
				       [&](auto &rLab) { return rLab.getPos() == symmPos; });
	    }))
		return true;
	return false;
}

bool GenMCDriver::isEcoSymmetric(const EventLabel *lab, int tid) const
{
	auto &g = getGraph();

	auto *symmLab = g.getEventLabel(Event(tid, lab->getIndex()));
	if (auto *rLab = llvm::dyn_cast<ReadLabel>(lab)) {
		return rLab->getRf() == llvm::dyn_cast<ReadLabel>(symmLab)->getRf();
	}

	auto *wLab = llvm::dyn_cast<WriteLabel>(lab);
	BUG_ON(!wLab);
	return g.co_imm_succ(wLab) == llvm::dyn_cast<WriteLabel>(symmLab);
}

bool GenMCDriver::isPredSymmetryOK(const EventLabel *lab, int symm)
{
	auto &g = getGraph();

	BUG_ON(symm == -1);
	if (!sharePrefixSR(symm, lab->getPos()) || !g.containsPos(Event(symm, lab->getIndex())))
		return true;

	auto *symmLab = g.getEventLabel(Event(symm, lab->getIndex()));
	if (symmLab->getKind() != lab->getKind())
		return true;

	return !isEcoBefore(lab, symm);
}

bool GenMCDriver::isPredSymmetryOK(const EventLabel *lab)
{
	auto &g = getGraph();
	std::vector<int> preds;

	auto symm = getSymmPredTid(lab->getThread());
	while (symm != -1) {
		preds.push_back(symm);
		symm = getSymmPredTid(symm);
	}
	return std::all_of(preds.begin(), preds.end(),
			   [&](auto &symm) { return isPredSymmetryOK(lab, symm); });
}

bool GenMCDriver::isSuccSymmetryOK(const EventLabel *lab, int symm)
{
	auto &g = getGraph();

	BUG_ON(symm == -1);
	if (!sharePrefixSR(symm, lab->getPos()) || !g.containsPos(Event(symm, lab->getIndex())))
		return true;

	auto *symmLab = g.getEventLabel(Event(symm, lab->getIndex()));
	if (symmLab->getKind() != lab->getKind())
		return true;

	return !isEcoBefore(symmLab, lab->getThread());
}

bool GenMCDriver::isSuccSymmetryOK(const EventLabel *lab)
{
	auto &g = getGraph();
	std::vector<int> succs;

	auto symm = getSymmSuccTid(lab->getThread());
	while (symm != -1) {
		succs.push_back(symm);
		symm = getSymmSuccTid(symm);
	}
	return std::all_of(succs.begin(), succs.end(),
			   [&](auto &symm) { return isSuccSymmetryOK(lab, symm); });
}

bool GenMCDriver::isSymmetryOK(const EventLabel *lab)
{
	auto &g = getGraph();
	return isPredSymmetryOK(lab) && isSuccSymmetryOK(lab);
}

void GenMCDriver::updatePrefixWithSymmetriesSR(EventLabel *lab)
{
	auto t = getSymmPredTid(lab->getThread());
	if (t == -1)
		return;

	auto &v = lab->getPrefixView();
	auto si = calcLargestSymmPrefixBeforeSR(t, lab->getPos());
	auto *symmLab = getGraph().getEventLabel({t, si});
	v.update(getPrefixView(symmLab));
	if (auto *rLab = llvm::dyn_cast<ReadLabel>(symmLab)) {
		v.update(getPrefixView(rLab->getRf()));
	}
}

int GenMCDriver::calcLargestSymmPrefixBeforeSR(int tid, Event pos) const
{
	auto &g = getGraph();

	if (tid < 0 || tid >= g.getNumThreads())
		return -1;

	auto limit = std::min((long)pos.index, (long)g.getThreadSize(tid) - 1);
	for (auto j = 0; j < limit; j++) {
		auto *labA = g.getEventLabel(Event(tid, j));
		auto *labB = g.getEventLabel(Event(pos.thread, j));

		if (labA->getKind() != labB->getKind())
			return j - 1;
		if (auto *rLabA = llvm::dyn_cast<ReadLabel>(labA)) {
			auto *rLabB = llvm::dyn_cast<ReadLabel>(labB);
			if (rLabA->getRf()->getThread() == tid &&
			    rLabB->getRf()->getThread() == pos.thread &&
			    rLabA->getRf()->getIndex() == rLabB->getRf()->getIndex())
				continue;
			if (rLabA->getRf() != rLabB->getRf())
				return j - 1;
		}
		if (auto *wLabA = llvm::dyn_cast<WriteLabel>(labA))
			if (!wLabA->isLocal())
				return j - 1;
	}
	return limit;
}

bool GenMCDriver::sharePrefixSR(int tid, Event pos) const
{
	return calcLargestSymmPrefixBeforeSR(tid, pos) == pos.index;
}

void GenMCDriver::filterSymmetricStoresSR(const ReadLabel *rLab, std::vector<Event> &stores) const
{
	auto &g = getGraph();
	auto t = getSymmPredTid(rLab->getThread());

	/* If there is no symmetric thread, exit */
	if (t == -1)
		return;

	/* Check whether the po-prefixes of the two threads match */
	if (!sharePrefixSR(t, rLab->getPos()))
		return;

	/* Get the symmetric event and make sure it matches as well */
	auto *lab = llvm::dyn_cast<ReadLabel>(g.getEventLabel(Event(t, rLab->getIndex())));
	if (!lab || lab->getAddr() != rLab->getAddr() || lab->getSize() != lab->getSize())
		return;

	if (!lab->isRMW())
		return;

	/* Remove stores that will be explored symmetrically */
	auto rfStamp = lab->getRf()->getStamp();
	stores.erase(std::remove_if(stores.begin(), stores.end(),
				    [&](auto s) { return lab->getRf()->getPos() == s; }),
		     stores.end());
	return;
}

void GenMCDriver::filterValuesFromAnnotSAVER(const ReadLabel *rLab, std::vector<Event> &validStores)
{
	/* Locks are treated as annotated CASes */
	if (!rLab->getAnnot())
		return;

	using Evaluator = SExprEvaluator<ModuleID::ID>;

	auto &g = getGraph();

	/* Ensure we keep the maximal store around even if Helper messed with it */
	BUG_ON(validStores.empty());
	auto maximal = validStores.back();
	validStores.erase(
		std::remove_if(validStores.begin(), validStores.end(),
			       [&](Event w) {
				       auto *wLab = g.getEventLabel(w);
				       auto val = getWriteValue(wLab, rLab->getAccess());
				       return w != maximal && wLab != g.co_max(rLab->getAddr()) &&
					      !Evaluator().evaluate(rLab->getAnnot(), val);
			       }),
		validStores.end());
	BUG_ON(validStores.empty());
}

void GenMCDriver::unblockWaitingHelping(const WriteLabel *lab)
{
	if (!llvm::isa<HelpedCasWriteLabel>(lab))
		return;

	/* FIXME: We have to wake up all threads waiting on helping CASes,
	 * as we don't know which ones are from the same CAS */
	for (auto i = 0u; i < getGraph().getNumThreads(); i++) {
		auto *bLab = llvm::dyn_cast<HelpedCASBlockLabel>(getGraph().getLastThreadLabel(i));
		if (bLab)
			getGraph().removeLast(bLab->getThread());
	}
}

bool GenMCDriver::writesBeforeHelpedContainedInView(const HelpedCasReadLabel *lab, const View &view)
{
	auto &g = getGraph();
	auto &hb = getConsChecker().getHbView(lab);

	for (auto i = 0u; i < hb.size(); i++) {
		auto j = hb.getMax(i);
		while (!llvm::isa<WriteLabel>(g.getEventLabel(Event(i, j))) && j > 0)
			--j;
		if (j > 0 && !view.contains(Event(i, j)))
			return false;
	}
	return true;
}

bool GenMCDriver::checkHelpingCasCondition(const HelpingCasLabel *hLab)
{
	auto &g = getGraph();

	auto hsView = g.labels() | std::views::filter([&g, hLab](auto &lab) {
			      auto *rLab = llvm::dyn_cast<HelpedCasReadLabel>(&lab);
			      return rLab && rLab->isRMW() && rLab->getAddr() == hLab->getAddr() &&
				     rLab->getType() == hLab->getType() &&
				     rLab->getSize() == hLab->getSize() &&
				     rLab->getOrdering() == hLab->getOrdering() &&
				     rLab->getExpected() == hLab->getExpected() &&
				     rLab->getSwapVal() == hLab->getSwapVal();
		      });

	if (std::ranges::any_of(hsView, [&g, this](auto &lab) {
		    auto *hLab = llvm::dyn_cast<HelpedCasReadLabel>(&lab);
		    auto &view = getConsChecker().getHbView(hLab);
		    return !writesBeforeHelpedContainedInView(hLab, view);
	    }))
		ERROR("Helped/Helping CAS annotation error! "
		      "Not all stores before helped-CAS are visible to helping-CAS!\n");
	return std::ranges::begin(hsView) != std::ranges::end(hsView);
}

bool GenMCDriver::checkAtomicity(const WriteLabel *wLab)
{
	if (getGraph().violatesAtomicity(wLab)) {
		moot();
		return false;
	}
	return true;
}

std::optional<Event> GenMCDriver::findConsistentRf(ReadLabel *rLab, std::vector<Event> &rfs)
{
	auto &g = getGraph();

	/* For the non-bounding case, maximal extensibility guarantees consistency */
	if (!getConf()->bound.has_value()) {
		rLab->setRf(g.getEventLabel(rfs.back()));
		return {rfs.back()};
	}

	/* Otherwise, search for a consistent rf */
	while (!rfs.empty()) {
		rLab->setRf(g.getEventLabel(rfs.back()));
		if (isExecutionValid(rLab))
			return {rfs.back()};
		rfs.erase(rfs.end() - 1);
	}

	/* If none is found, tough luck */
	moot();
	return std::nullopt;
}

std::optional<Event> GenMCDriver::findConsistentCo(WriteLabel *wLab, std::vector<Event> &cos)
{
	auto &g = getGraph();

	/* Similarly to the read case: rely on extensibility */
	g.addStoreToCOAfter(wLab, g.getEventLabel(cos.back()));
	if (!getConf()->bound.has_value())
		return {cos.back()};

	/* In contrast to the read case, we need to be a bit more careful:
	 * the consistent choice might not satisfy atomicity, but we should
	 * keep it around to try revisits */
	while (!cos.empty()) {
		g.moveStoreCOAfter(wLab, g.getEventLabel(cos.back()));
		if (isExecutionValid(wLab))
			return {cos.back()};
		cos.erase(cos.end() - 1);
	}
	moot();
	return std::nullopt;
}

void GenMCDriver::handleThreadKill(std::unique_ptr<ThreadKillLabel> kLab)
{
	BUG_ON(isExecutionDrivenByGraph(&*kLab));
	addLabelToGraph(std::move(kLab));
	return;
}

bool GenMCDriver::isSymmetricToSR(int candidate, Event parent, const ThreadInfo &info) const
{
	auto &g = getGraph();
	auto cParent = g.getFirstThreadLabel(candidate)->getParentCreate();
	auto &cInfo = g.getFirstThreadLabel(candidate)->getThreadInfo();

	/* A tip to print to the user in case two threads look
	 * symmetric, but we cannot deem it */
	auto tipSymmetry = [&]() {
		LOG_ONCE("possible-symmetry", VerbosityLevel::Tip)
			<< "Threads (" << getEE()->getThrById(cInfo.id) << ") and ("
			<< getEE()->getThrById(info.id)
			<< ") could benefit from symmetry reduction."
			<< " Consider using __VERIFIER_spawn_symmetric().\n";
	};

	/* First, check that the two threads are actually similar */
	if (cInfo.id == info.id || cInfo.parentId != info.parentId || cInfo.funId != info.funId ||
	    cInfo.arg != info.arg) {
		if (cInfo.funId == info.funId && cInfo.parentId == info.parentId)
			tipSymmetry();
		return false;
	}

	/* Then make sure that there is no memory access in between the spawn events */
	auto mm = std::minmax(parent.index, cParent.index);
	auto minI = mm.first;
	auto maxI = mm.second;
	for (auto j = minI; j < maxI; j++) {
		if (llvm::isa<MemAccessLabel>(g.getEventLabel(Event(parent.thread, j)))) {
			tipSymmetry();
			return false;
		}
	}
	return true;
}

int GenMCDriver::getSymmetricTidSR(const ThreadCreateLabel *tcLab,
				   const ThreadInfo &childInfo) const
{
	if (!getConf()->symmetryReduction)
		return -1;

	/* Has the user provided any info? */
	if (childInfo.symmId != -1)
		return childInfo.symmId;

	auto &g = getGraph();

	for (auto i = childInfo.id - 1; i > 0; i--)
		if (isSymmetricToSR(i, tcLab->getPos(), childInfo))
			return i;
	return -1;
}

int GenMCDriver::handleThreadCreate(std::unique_ptr<ThreadCreateLabel> tcLab)
{
	auto &g = getGraph();
	auto *EE = getEE();

	if (isExecutionDrivenByGraph(&*tcLab))
		return llvm::dyn_cast<ThreadCreateLabel>(g.getEventLabel(tcLab->getPos()))
			->getChildId();

	/* First, check if the thread to be created already exists */
	int cid = 0;
	while (cid < (long)g.getNumThreads()) {
		if (!g.isThreadEmpty(cid)) {
			auto *bLab = llvm::dyn_cast<ThreadStartLabel>(g.getFirstThreadLabel(cid));
			BUG_ON(!bLab);
			if (bLab->getParentCreate() == tcLab->getPos())
				break;
		}
		++cid;
	}

	/* Add an event for the thread creation */
	tcLab->setChildId(cid);
	auto *lab = llvm::dyn_cast<ThreadCreateLabel>(addLabelToGraph(std::move(tcLab)));

	/* Prepare the execution context for the new thread */
	EE->constructAddThreadFromInfo(lab->getChildInfo());

	/* If the thread does not exist in the graph, make an entry for it */
	if (cid == (long)g.getNumThreads()) {
		g.addNewThread();
		BUG_ON(EE->getNumThreads() != g.getNumThreads());
	} else {
		BUG_ON(g.getThreadSize(cid) != 1);
		g.removeLast(cid);
	}
	auto symm = getSymmetricTidSR(lab, lab->getChildInfo());
	auto tsLab =
		ThreadStartLabel::create(Event(cid, 0), lab->getPos(), lab->getChildInfo(), symm);
	addLabelToGraph(std::move(tsLab));
	return cid;
}

std::optional<SVal> GenMCDriver::handleThreadJoin(std::unique_ptr<ThreadJoinLabel> lab)
{
	auto &g = getGraph();

	if (isExecutionDrivenByGraph(&*lab))
		return {getJoinValue(
			llvm::dyn_cast<ThreadJoinLabel>(g.getEventLabel(lab->getPos())))};

	if (!llvm::isa<ThreadFinishLabel>(g.getLastThreadLabel(lab->getChildId()))) {
		blockThread(JoinBlockLabel::create(lab->getPos(), lab->getChildId()));
		return std::nullopt;
	}

	auto *jLab = llvm::dyn_cast<ThreadJoinLabel>(addLabelToGraph(std::move(lab)));
	auto cid = jLab->getChildId();

	auto *eLab = llvm::dyn_cast<ThreadFinishLabel>(g.getLastThreadLabel(cid));
	BUG_ON(!eLab);
	eLab->setParentJoin(jLab);

	if (cid < 0 || long(g.getNumThreads()) <= cid || cid == jLab->getThread()) {
		std::string err = "ERROR: Invalid TID in pthread_join(): " + std::to_string(cid);
		if (cid == jLab->getThread())
			err += " (TID cannot be the same as the calling thread)";
		reportError({jLab->getPos(), VerificationError::VE_InvalidJoin, err});
		return {SVal(0)};
	}

	if (partialExecutionExceedsBound()) {
		moot();
		return std::nullopt;
	}

	return {getJoinValue(jLab)};
}

void GenMCDriver::handleThreadFinish(std::unique_ptr<ThreadFinishLabel> eLab)
{
	auto &g = getGraph();

	if (isExecutionDrivenByGraph(&*eLab))
		return;

	auto *lab = addLabelToGraph(std::move(eLab));
	for (auto i = 0U; i < g.getNumThreads(); i++) {
		auto *pLab = llvm::dyn_cast<JoinBlockLabel>(g.getLastThreadLabel(i));
		if (pLab && pLab->getChildId() == lab->getThread()) {
			/* If parent thread is waiting for me, relieve it */
			unblockThread(pLab->getPos());
		}
	}
	if (partialExecutionExceedsBound())
		moot();
}

void GenMCDriver::handleFence(std::unique_ptr<FenceLabel> fLab)
{
	if (isExecutionDrivenByGraph(&*fLab))
		return;

	addLabelToGraph(std::move(fLab));
}

void GenMCDriver::checkReconsiderFaiSpinloop(const MemAccessLabel *lab)
{
	auto &g = getGraph();
	auto *EE = getEE();

	for (auto i = 0u; i < g.getNumThreads(); i++) {
		/* Is there any thread blocked on a potential spinloop? */
		auto *eLab = llvm::dyn_cast<FaiZNEBlockLabel>(g.getLastThreadLabel(i));
		if (!eLab)
			continue;

		/* Check whether this access affects the spinloop variable */
		auto epreds = po_preds(g, eLab);
		auto faiLabIt = std::ranges::find_if(
			epreds, [](auto &lab) { return llvm::isa<FaiWriteLabel>(&lab); });
		BUG_ON(faiLabIt == std::ranges::end(epreds));

		auto *faiLab = llvm::dyn_cast<FaiWriteLabel>(&*faiLabIt);
		if (faiLab->getAddr() != lab->getAddr())
			continue;

		/* FAIs on the same variable are OK... */
		if (llvm::isa<FaiReadLabel>(lab) || llvm::isa<FaiWriteLabel>(lab))
			continue;

		/* If it does, and also breaks the assumptions, unblock thread */
		if (!getConsChecker().getHbView(faiLab).contains(lab->getPos())) {
			auto pos = eLab->getPos();
			unblockThread(pos);
			addLabelToGraph(FaiZNESpinEndLabel::create(pos));
		}
	}
}

const VectorClock &GenMCDriver::getPrefixView(const EventLabel *lab) const
{
	// FIXME
	if (!lab->hasPrefixView())
		lab->setPrefixView(const_cast<ConsistencyChecker &>(getConsChecker())
					   .calculatePrefixView(lab));
	return lab->getPrefixView();
}

std::vector<Event> GenMCDriver::getRfsApproximation(const ReadLabel *lab)
{
	auto &g = getGraph();
	auto &cc = getConsChecker();
	auto rfs = cc.getCoherentStores(g, lab->getAddr(), lab->getPos());
	if (!llvm::isa<CasReadLabel>(lab) && !llvm::isa<FaiReadLabel>(lab))
		return rfs;

	/* Remove atomicity violations */
	auto &before = getPrefixView(lab);
	auto isSettledRMWInView = [](auto &rLab, auto &before) {
		auto &g = *rLab.getParent();
		return rLab.isRMW() &&
		       ((!rLab.isRevisitable() && !llvm::dyn_cast<WriteLabel>(g.getNextLabel(&rLab))
							   ->hasAttr(WriteAttr::RevBlocker)) ||
			before.contains(rLab.getPos()));
	};
	auto storeReadBySettledRMWInView = [&isSettledRMWInView](auto *sLab, auto &before,
								 SAddr addr) {
		if (auto *wLab = llvm::dyn_cast<WriteLabel>(sLab)) {
			return std::ranges::any_of(wLab->readers(), [&](auto &rLab) {
				return isSettledRMWInView(rLab, before);
			});
		};

		auto *iLab = llvm::dyn_cast<InitLabel>(sLab);
		BUG_ON(!iLab);
		return std::ranges::any_of(iLab->rfs(addr), [&](auto &rLab) {
			return isSettledRMWInView(rLab, before);
		});
	};
	rfs.erase(std::remove_if(rfs.begin(), rfs.end(),
				 [&](const Event &s) {
					 auto *sLab = g.getEventLabel(s);
					 auto oldVal = getWriteValue(sLab, lab->getAccess());
					 return lab->valueMakesRMWSucceed(oldVal) &&
						storeReadBySettledRMWInView(sLab, before,
									    lab->getAddr());
				 }),
		  rfs.end());
	return rfs;
}

void GenMCDriver::filterOptimizeRfs(const ReadLabel *lab, std::vector<Event> &stores)
{
	/* Symmetry reduction */
	if (getConf()->symmetryReduction)
		filterSymmetricStoresSR(lab, stores);

	/* BAM */
	if (!getConf()->disableBAM)
		filterConflictingBarriers(lab, stores);

	/* Keep values that do not lead to blocking */
	filterValuesFromAnnotSAVER(lab, stores);
}

void GenMCDriver::filterAtomicityViolations(const ReadLabel *rLab, std::vector<Event> &stores)
{
	auto &g = getGraph();
	if (!llvm::isa<CasReadLabel>(rLab) && !llvm::isa<FaiReadLabel>(rLab))
		return;

	const auto *casLab = llvm::dyn_cast<CasReadLabel>(rLab);
	auto valueMakesSuccessfulRMW = [&casLab, rLab](auto &&val) {
		return !casLab || val == casLab->getExpected();
	};
	stores.erase(std::remove_if(
			     stores.begin(), stores.end(),
			     [&](auto &s) {
				     auto *sLab = g.getEventLabel(s);
				     if (auto *iLab = llvm::dyn_cast<InitLabel>(sLab))
					     return std::any_of(
						     iLab->rf_begin(rLab->getAddr()),
						     iLab->rf_end(rLab->getAddr()),
						     [&](auto &rLab) {
							     return rLab.isRMW() &&
								    valueMakesSuccessfulRMW(
									    getReadValue(&rLab));
						     });
				     return std::any_of(rf_succ_begin(g, sLab),
							rf_succ_end(g, sLab), [&](auto &rLab) {
								return rLab.isRMW() &&
								       valueMakesSuccessfulRMW(
									       getReadValue(&rLab));
							});
			     }),
		     stores.end());
}

void GenMCDriver::updateStSpaceChoices(const ReadLabel *rLab, const std::vector<Event> &stores)
{
	auto &choices = getChoiceMap();
	choices[rLab->getStamp()].clear();
	for (auto &s : stores) {
		choices[rLab->getStamp()].insert(std::make_pair(s, -1));
	}
}

Event GenMCDriver::pickRf(ReadLabel *rLab, std::vector<Event> &stores, bool pickEnd)
{
	auto &g = getGraph();

	stores.erase(std::remove_if(stores.begin(), stores.end(),
				    [&](auto &s) {
					    rLab->setRf(g.getEventLabel(s));
					    return !isExecutionValid(rLab);
				    }),
		     stores.end());

	auto errorIt = std::ranges::find_if(
		stores, [&](auto &s) { return isUninitializedAccess(rLab->getAddr(), s); });
	if (errorIt != std::ranges::end(stores)) {
		rLab->setRf(g.getEventLabel(*errorIt));
		reportError({rLab->getPos(), VerificationError::VE_UninitializedMem});
		return *errorIt;
	}

	size_t idx;
	if (pickEnd) {
		idx = stores.size() - 1;
	} else {
		MyDist dist(0, stores.size() - 1);
		idx = inEstimationMode() ? dist(estRng) : dist(rng);
	}

	if (getConf()->interactiveAddGraph) {
		llvm::dbgs() << "handling load: " << llvm::raw_ostream::Colors::RED
			     << rLab->getPos();
		llvm::dbgs().resetColor();
		llvm::dbgs() << '\n';
		if (!getConf()->dotFile.empty()) {
			fuzzPreviewCurGraph();
		}
		llvm::dbgs() << "\tRfs : [";
		for (int i = 0; i < stores.size(); i++) {
			llvm::dbgs() << llvm::raw_ostream::Colors::GREEN << i;
			llvm::dbgs().resetColor();
			llvm::dbgs() << ": " << stores[i] << " ";
		}
		llvm::dbgs() << "]\n";

		if (stores.size() > 1) {
			do {
				llvm::dbgs() << ">>> ";
				std::cin >> idx;
			} while (!(idx >= 0 && idx < stores.size()));
			llvm::dbgs()
				<< "\tpicked " << llvm::raw_ostream::Colors::GREEN << stores[idx];
			llvm::dbgs().resetColor();
		}
	}

	rLab->setRf(g.getEventLabel(stores[idx]));

	return stores[idx];
}

Event GenMCDriver::pickRandomRf(ReadLabel *rLab, std::vector<Event> &stores)
{
	return pickRf(rLab, stores, false);
}

bool GenMCDriver::shouldPickCoRfRandomly() const
{
	if (inEstimationMode())
		return true;
	if (inFuzzingMode()) {
		switch (getConf()->fuzzAddMaxCoRf) {
		case AddMaxCoRf::Never:
			return true;
		case AddMaxCoRf::Empty:
			return !(lastGEmpty);
		case AddMaxCoRf::Mutated:
			return lastGEmpty;
		default:
			return false;
		}
	}
	return false;
}

std::optional<SVal> GenMCDriver::handleLoad(std::unique_ptr<ReadLabel> rLab)
{
	auto &g = getGraph();
	auto *EE = getEE();

	if (inRecoveryMode() && rLab->getAddr().isVolatile())
		return {getRecReadRetValue(rLab.get())};

	if (isExecutionDrivenByGraph(&*rLab))
		return getReadRetValue(llvm::dyn_cast<ReadLabel>(g.getEventLabel(rLab->getPos())));

	if (!rLab->getAnnot())
		rLab->setAnnot(EE->getCurrentAnnotConcretized());
	auto *lab = llvm::dyn_cast<ReadLabel>(addLabelToGraph(std::move(rLab)));

	if (checkAccessValidity(lab) != VerificationError::VE_OK ||
	    checkForRaces(lab) != VerificationError::VE_OK ||
	    checkIPRValidity(lab) != VerificationError::VE_OK)
		return std::nullopt; /* This execution will be blocked */

	/* Check whether the load forces us to reconsider some existing event */
	checkReconsiderFaiSpinloop(lab);

	/* If a CAS read cannot be added maximally, reschedule */
	if (!isRescheduledRead(lab->getPos()) &&
	    removeCASReadIfBlocks(lab, g.co_max(lab->getAddr())))
		return std::nullopt;
	if (isRescheduledRead(lab->getPos()))
		setRescheduledRead(Event::getInit());

	/* Get an approximation of the stores we can read from */
	auto stores = getRfsApproximation(lab);
	BUG_ON(stores.empty());

	if (inFuzzingMode()) {
		auto [vals, last] = extractValPrefix(lab->getPos());

		// FIXME: go over all stores
		vals.push_back(SVal(0)); // dummy value
		for (const auto &val : stores | std::views::transform([&](auto &s) {
					       return getWriteValue(g.getEventLabel(s),
								    lab->getAccess());
				       })) {
			vals.pop_back();
			vals.push_back(val);
			auto *data = seenValues[lab->getThread()].lookup(vals);
			if (!data) {
				seenValues[lab->getThread()].addSeq(vals, 0);
			}
		}
	}

	GENMC_DEBUG(LOG(VerbosityLevel::Debug3) << "Rfs: " << format(stores) << "\n";);
	filterOptimizeRfs(lab, stores);
	GENMC_DEBUG(LOG(VerbosityLevel::Debug3) << "Rfs (optimized): " << format(stores) << "\n";);

	std::optional<Event> rf = std::nullopt;

	if (inEstimationMode() || inFuzzingMode()) {
		updateStSpaceChoices(lab, stores);
		filterAtomicityViolations(lab, stores);
		rf = shouldPickCoRfRandomly() ? pickRandomRf(lab, stores)
					      : pickRf(lab, stores, true);
	} else {
		rf = findConsistentRf(lab, stores);
		/* Push all the other alternatives choices to the Stack (many maximals for
		 * wb) */
		for (const auto &s : stores | std::views::take(stores.size() - 1)) {
			auto status = false; /* MO messes with the status */
			addToWorklist(lab->getStamp(), std::make_unique<ReadForwardRevisit>(
							       lab->getPos(), s, status));
		}
	}

	/* Ensured the selected rf comes from an initialized memory location */
	if (!rf.has_value() || checkInitializedMem(lab) != VerificationError::VE_OK)
		return std::nullopt; /* nullopt case under bounding only */

	GENMC_DEBUG(LOG(VerbosityLevel::Debug2) << "--- Added load " << lab->getPos() << "\n"
						<< getGraph(););

	/* If this is the last part of barrier_wait() check whether we should block */
	auto retVal = getWriteValue(g.getEventLabel(*rf), lab->getAccess());
	if (llvm::isa<BWaitReadLabel>(lab) && retVal != getBarrierInitValue(lab->getAccess())) {
		blockThread(BarrierBlockLabel::create(lab->getPos().next()));
	}
	return {retVal};
}

void GenMCDriver::annotateStoreHELPER(WriteLabel *wLab)
{
	auto &g = getGraph();

	/* Don't bother with lock ops */
	if (!getConf()->helper || !wLab->isRMW() || llvm::isa<LockCasWriteLabel>(wLab) ||
	    llvm::isa<TrylockCasWriteLabel>(wLab))
		return;

	/* Check whether we can mark it as RevBlocker */
	auto *pLab = g.getPreviousLabel(wLab);
	auto *mLab = llvm::dyn_cast_or_null<MemAccessLabel>(
		getPreviousVisibleAccessLabel(pLab->getPos()));
	auto *rLab = llvm::dyn_cast_or_null<ReadLabel>(mLab);
	if (!mLab || (mLab->wasAddedMax() && (!rLab || rLab->isRevisitable())))
		return;

	/* Mark the store and its predecessor */
	if (llvm::isa<FaiWriteLabel>(wLab))
		llvm::dyn_cast<FaiReadLabel>(pLab)->setAttr(WriteAttr::RevBlocker);
	else
		llvm::dyn_cast<CasReadLabel>(pLab)->setAttr(WriteAttr::RevBlocker);
	wLab->setAttr(WriteAttr::RevBlocker);
}

std::vector<Event> GenMCDriver::getRevisitableApproximation(const WriteLabel *sLab)
{
	auto &g = getGraph();
	auto &prefix = getPrefixView(sLab);
	auto loads = getConsChecker().getCoherentRevisits(g, sLab, prefix);
	std::sort(loads.begin(), loads.end(), [&g](const Event &l1, const Event &l2) {
		return g.getEventLabel(l1)->getStamp() > g.getEventLabel(l2)->getStamp();
	});
	return loads;
}

void GenMCDriver::pickCo(WriteLabel *sLab, std::vector<Event> &cos, bool pickEnd)
{
	auto &g = getGraph();

	g.addStoreToCOAfter(sLab, g.getEventLabel(cos.back()));
	cos.erase(std::remove_if(cos.begin(), cos.end(),
				 [&](auto &s) {
					 g.moveStoreCOAfter(sLab, g.getEventLabel(s));
					 return !isExecutionValid(sLab);
				 }),
		  cos.end());

	/* Extensibility is not guaranteed if an RMW read is not reading maximally
	 * (during estimation, reads read from arbitrary places anyway).
	 * If that is the case, we have to ensure that estimation won't stop. */
	if (cos.empty()) {
		moot();
		addToWorklist(0, std::make_unique<RerunForwardRevisit>());
		return;
	}

	size_t idx;
	if (pickEnd) {
		idx = cos.size() - 1;
	} else {
		MyDist dist(0, cos.size() - 1);
		idx = inEstimationMode() ? dist(estRng) : dist(rng);
	}

	if (getConf()->interactiveAddGraph && cos.size() > 1) {
		llvm::dbgs() << "handling store: " << llvm::raw_ostream::Colors::RED
			     << sLab->getPos();
		llvm::dbgs().resetColor();
		llvm::dbgs() << '\n';
		if (!getConf()->dotFile.empty()) {
			fuzzPreviewCurGraph();
		}
		llvm::dbgs() << "\tCos : [";
		for (int i = 0; i < cos.size(); i++) {
			llvm::dbgs() << llvm::raw_ostream::Colors::GREEN << i;
			llvm::dbgs().resetColor();
			llvm::dbgs() << ": " << cos[i] << " ";
		}
		llvm::dbgs() << "]\n";
		do {
			llvm::dbgs() << ">>> ";
			std::cin >> idx;
		} while (!(idx >= 0 && idx < cos.size()));
		llvm::dbgs() << "\tinsert after " << llvm::raw_ostream::Colors::GREEN << cos[idx];
		llvm::dbgs().resetColor();
	}

	g.moveStoreCOAfter(sLab, g.getEventLabel(cos[idx]));
}

void GenMCDriver::pickRandomCo(WriteLabel *sLab, std::vector<Event> &cos) { pickCo(sLab, cos); }

void GenMCDriver::updateStSpaceChoices(const WriteLabel *wLab, const std::vector<Event> &stores)
{
	auto &choices = getChoiceMap();
	choices[wLab->getStamp()].clear();
	for (auto &s : stores) {
		choices[wLab->getStamp()].insert(std::make_pair(s, -1));
	}

	// choices[wLab->getStamp()] = stores;
}

void GenMCDriver::calcCoOrderings(WriteLabel *lab, const std::vector<Event> &cos)
{
	auto &g = getGraph();

	for (auto &pred : cos | std::views::take(cos.size() - 1)) {
		addToWorklist(lab->getStamp(),
			      std::make_unique<WriteForwardRevisit>(lab->getPos(), pred));
	}
}

void GenMCDriver::handleStore(std::unique_ptr<WriteLabel> wLab)
{
	if (isExecutionDrivenByGraph(&*wLab))
		return;

	auto &g = getGraph();

	if (getConf()->helper && wLab->isRMW())
		annotateStoreHELPER(&*wLab);
	if (llvm::isa<BIncFaiWriteLabel>(&*wLab) && wLab->getVal() == SVal(0))
		wLab->setVal(getBarrierInitValue(wLab->getAccess()));

	auto *lab = llvm::dyn_cast<WriteLabel>(addLabelToGraph(std::move(wLab)));

	if (checkAccessValidity(lab) != VerificationError::VE_OK ||
	    checkInitializedMem(lab) != VerificationError::VE_OK ||
	    checkFinalAnnotations(lab) != VerificationError::VE_OK ||
	    checkForRaces(lab) != VerificationError::VE_OK)
		return;

	checkReconsiderFaiSpinloop(lab);
	unblockWaitingHelping(lab);
	checkReconsiderReadOpts(lab);

	/* Find all possible placings in coherence for this store, and
	 * print a WW-race warning if appropriate (if this moots,
	 * exploration will anyway be cut) */
	auto cos = getConsChecker().getCoherentPlacings(g, lab->getAddr(), lab->getPos(),
							lab->isRMW());
	if (cos.size() > 1) {
		reportWarningOnce(lab->getPos(), VerificationError::VE_WWRace,
				  g.getEventLabel(cos[0]));
	}

	std::optional<Event> co;
	if (inEstimationMode()) {
		pickRandomCo(lab, cos);
		updateStSpaceChoices(lab, cos);
	} else if (inFuzzingMode()) {
		shouldPickCoRfRandomly() ? pickRandomCo(lab, cos) : pickCo(lab, cos, true);
		updateStSpaceChoices(lab, cos);
		if (getConf()->mutation == MutationPolicy::NoMutation)
			return;
	} else {
		co = findConsistentCo(lab, cos);
		calcCoOrderings(lab, cos);
	}

	GENMC_DEBUG(LOG(VerbosityLevel::Debug2) << "--- Added store " << lab->getPos() << "\n"
						<< getGraph(););

	if (inRecoveryMode() || inReplay())
		return;
	calcRevisits(lab);
}

SVal GenMCDriver::handleMalloc(std::unique_ptr<MallocLabel> aLab)
{
	auto &g = getGraph();

	if (isExecutionDrivenByGraph(&*aLab)) {
		auto *lab = llvm::dyn_cast<MallocLabel>(g.getEventLabel(aLab->getPos()));
		BUG_ON(!lab);
		return SVal(lab->getAllocAddr().get());
	}

	/* Fix and add label to the graph; return the new address */
	if (aLab->getAllocAddr() == SAddr())
		aLab->setAllocAddr(getFreshAddr(&*aLab));
	auto *lab = llvm::dyn_cast<MallocLabel>(addLabelToGraph(std::move(aLab)));
	return SVal(lab->getAllocAddr().get());
}

void GenMCDriver::handleFree(std::unique_ptr<FreeLabel> dLab)
{
	auto &g = getGraph();

	if (isExecutionDrivenByGraph(&*dLab))
		return;

	/* Find the size of the area deallocated */
	auto size = 0u;
	auto alloc = findAllocatingLabel(g, dLab->getFreedAddr());
	if (alloc) {
		size = alloc->getAllocSize();
	}

	/* Add a label with the appropriate store */
	dLab->setFreedSize(size);
	dLab->setAlloc(alloc);
	auto *lab = addLabelToGraph(std::move(dLab));
	alloc->setFree(llvm::dyn_cast<FreeLabel>(lab));

	/* Check whether there is any memory race */
	checkForRaces(lab);
}

const MemAccessLabel *GenMCDriver::getPreviousVisibleAccessLabel(Event start) const
{
	auto &g = getGraph();
	std::vector<Event> finalReads;

	for (auto pos = start.prev(); pos.index > 0; --pos) {
		auto *lab = g.getEventLabel(pos);
		if (auto *rLab = llvm::dyn_cast<ReadLabel>(lab)) {
			if (getConf()->helper && rLab->isConfirming())
				continue;
			if (rLab->getRf()) {
				auto *wLab = llvm::dyn_cast<WriteLabel>(rLab->getRf());
				if (wLab && wLab->isLocal())
					continue;
				if (wLab && wLab->isFinal()) {
					finalReads.push_back(rLab->getPos());
					continue;
				}
				if (std::any_of(finalReads.begin(), finalReads.end(),
						[&](const Event &l) {
							auto *lLab = llvm::dyn_cast<ReadLabel>(
								g.getEventLabel(l));
							return lLab->getAddr() == rLab->getAddr() &&
							       lLab->getSize() == rLab->getSize();
						}))
					continue;
			}
			return rLab;
		}
		if (auto *wLab = llvm::dyn_cast<WriteLabel>(lab))
			if (!wLab->isFinal() && !wLab->isLocal())
				return wLab;
	}
	return nullptr; /* none found */
}

void GenMCDriver::mootExecutionIfFullyBlocked(Event pos)
{
	auto &g = getGraph();

	auto *lab = getPreviousVisibleAccessLabel(pos);
	if (auto *rLab = llvm::dyn_cast_or_null<ReadLabel>(lab))
		if (!rLab->isRevisitable() || !rLab->wasAddedMax())
			moot();
	return;
}

void GenMCDriver::handleBlock(std::unique_ptr<BlockLabel> lab)
{
	if (isExecutionDrivenByGraph(&*lab))
		return;

	/* Call addLabelToGraph first to cache the label */
	addLabelToGraph(lab->clone());
	blockThreadTryMoot(std::move(lab));
}

std::unique_ptr<VectorClock> GenMCDriver::getReplayView() const
{
	auto &g = getGraph();
	auto v = g.getViewFromStamp(g.getMaxStamp());

	/* handleBlock() is usually only called during normal execution
	 * and hence not reproduced during replays.
	 * We have to remove BlockLabels so that these will not lead
	 * to the execution of extraneous instructions */
	for (auto i = 0u; i < g.getNumThreads(); i++)
		if (llvm::isa<BlockLabel>(g.getLastThreadLabel(i)))
			v->setMax(Event(i, v->getMax(i) - 1));
	return v;
}

void GenMCDriver::reportError(const ErrorDetails &details)
{
	auto &g = getGraph();

	/* If we have already detected an error, no need to report another */
	if (isHalting())
		return;

	/* If we this is a replay (might happen if one LLVM instruction
	 * maps to many MC events), do not get into an infinite loop... */
	if (inReplay())
		return;

	/* Ignore soft errors under estimation mode.
	 * These are going to be reported later on anyway */
	if (!details.shouldHalt && inEstimationMode())
		return;

	/* If this is an invalid access, change the RF of the offending
	 * event to BOTTOM, so that we do not try to get its value.
	 * Don't bother updating the views */
	auto *errLab = g.getEventLabel(details.pos);
	if (isInvalidAccessError(details.type) && llvm::isa<ReadLabel>(errLab))
		llvm::dyn_cast<ReadLabel>(errLab)->setRf(nullptr);

	/* Print a basic error message and the graph.
	 * We have to save the interpreter state as replaying will
	 * destroy the current execution stack */
	auto iState = getEE()->saveState();

	getEE()->replayExecutionBefore(*getReplayView());

	llvm::raw_string_ostream out(result.message);

	out << (isHardError(details.type) ? "Error: " : "Warning: ") << details.type << "!\n";
	out << "Event " << errLab->getPos() << " ";
	if (details.racyLab != nullptr)
		out << "conflicts with event " << details.racyLab->getPos() << " ";
	out << "in graph:\n";
	printGraph(true, out);

	/* Print error trace leading up to the violating event(s) */
	if (getConf()->printErrorTrace) {
		printTraceBefore(errLab, out);
		if (details.racyLab != nullptr)
			printTraceBefore(details.racyLab, out);
	}

	/* Print the specific error message */
	if (!details.msg.empty())
		out << details.msg << "\n";

	/* Dump the graph into a file (DOT format) */
	if (!getConf()->dotFile.empty())
		dotPrintToFile(getConf()->dotFile, errLab, details.racyLab);

	getEE()->restoreState(std::move(iState));

	if (details.shouldHalt)
		halt(details.type);
}

bool GenMCDriver::reportWarningOnce(Event pos, VerificationError wcode,
				    const EventLabel *racyLab /* = nullptr */)
{
	/* Helper function to determine whether the warning should be treated as an error */
	auto shouldUpgradeWarning = [&](auto &wcode) {
		if (wcode != VerificationError::VE_WWRace)
			return std::make_pair(false, ""s);
		if (!getConf()->symmetryReduction && !getConf()->ipr)
			return std::make_pair(false, ""s);

		auto &g = getGraph();
		auto *lab = g.getEventLabel(pos);
		auto upgrade =
			(getConf()->symmetryReduction &&
			 std::ranges::any_of(
				 g.thr_ids(),
				 [&](auto tid) {
					 return g.getFirstThreadLabel(tid)->getSymmetricTid() != -1;
				 })) ||
			(getConf()->ipr &&
			 std::any_of(sameloc_begin(g, lab), sameloc_end(g, lab), [&](auto &oLab) {
				 auto *rLab = llvm::dyn_cast<ReadLabel>(&oLab);
				 return rLab && rLab->getAnnot();
			 }));
		auto [cause, cli] =
			getConf()->ipr
				? std::make_pair("in-place revisiting (IPR)"s, "-disable-ipr"s)
				: std::make_pair("symmetry reduction (SR)"s, "-disable-sr"s);
		auto msg = "Unordered writes do not constitute a bug per se, though they "
			   "often "
			   "indicate faulty design.\n" +
			   (upgrade ? ("This warning is treated as an error due to " + cause +
				       ".\n"
				       "You can use " +
				       cli + " to disable these features."s)
				    : ""s);
		return std::make_pair(upgrade, msg);
	};

	/* If the warning has been seen before, only report it if it's an error */
	auto [upgradeWarning, msg] = shouldUpgradeWarning(wcode);
	auto &knownWarnings = getResult().warnings;
	if (upgradeWarning || knownWarnings.count(wcode) == 0) {
		reportError({pos, wcode, msg, racyLab, upgradeWarning});
	}
	if (knownWarnings.count(wcode) == 0)
		knownWarnings.insert(wcode);
	if (wcode == VerificationError::VE_WWRace)
		getGraph().getWriteLabel(pos)->setAttr(WriteAttr::WWRacy);
	return upgradeWarning;
}

bool GenMCDriver::tryOptimizeBarrierRevisits(BIncFaiWriteLabel *sLab, std::vector<Event> &loads)
{
	if (getConf()->disableBAM)
		return false;

	/* If the barrier_wait() does not write the initial value, nothing to do */
	auto iVal = getBarrierInitValue(sLab->getAccess());
	if (sLab->getVal() != iVal)
		return true;

	/* Otherwise, revisit in place */
	auto &g = getGraph();
	auto bsView = g.labels() | std::views::filter([&g, sLab](auto &lab) {
			      if (!llvm::isa<BarrierBlockLabel>(&lab))
				      return false;
			      auto *pLab = llvm::dyn_cast<BIncFaiWriteLabel>(
				      g.getPreviousLabel(g.getPreviousLabel(&lab)));
			      return pLab->getAddr() == sLab->getAddr();
		      }) |
		      std::views::transform([](auto &lab) { return lab.getPos(); });
	std::vector<Event> bs(std::ranges::begin(bsView), std::ranges::end(bsView));
	auto unblockedLoads = std::count_if(loads.begin(), loads.end(), [&](auto &l) {
		auto *nLab = llvm::dyn_cast_or_null<BlockLabel>(g.getNextLabel(g.getEventLabel(l)));
		return !nLab;
	});
	if (bs.size() > iVal.get() || unblockedLoads > 0)
		WARN_ONCE("bam-well-formed", "Execution not barrier-well-formed!\n");

	for (auto &b : bs) {
		auto *pLab = llvm::dyn_cast<BIncFaiWriteLabel>(
			g.getPreviousLabel(g.getPreviousLabel(g.getEventLabel(b))));
		BUG_ON(!pLab);
		unblockThread(b);
		g.removeLast(b.thread);
		auto *rLab = llvm::dyn_cast<ReadLabel>(addLabelToGraph(
			BWaitReadLabel::create(b.prev(), pLab->getOrdering(), pLab->getAddr(),
					       pLab->getSize(), pLab->getType(), pLab->getDeps())));
		rLab->setRf(sLab);
		rLab->setAddedMax(rLab->getRf() == g.co_max(rLab->getAddr()));
	}
	return true;
}

void GenMCDriver::tryOptimizeIPRs(const WriteLabel *sLab, std::vector<Event> &loads)
{
	if (!getConf()->ipr)
		return;

	auto &g = getGraph();

	std::vector<Event> toIPR;
	loads.erase(std::remove_if(loads.begin(), loads.end(),
				   [&](auto &l) {
					   auto *rLab = g.getReadLabel(l);
					   /* Treatment of blocked CASes is different */
					   auto blocked = !llvm::isa<CasReadLabel>(rLab) &&
							  rLab->getAnnot() &&
							  !rLab->valueMakesAssumeSucceed(
								  getReadValue(rLab));
					   if (blocked)
						   toIPR.push_back(l);
					   return blocked;
				   }),
		    loads.end());

	for (auto &l : toIPR)
		revisitInPlace(*constructBackwardRevisit(g.getReadLabel(l), sLab));

	/* We also have to filter out some regular revisits */
	auto pending = g.getPendingRMW(sLab);
	if (!pending.isInitializer()) {
		loads.erase(std::remove_if(loads.begin(), loads.end(),
					   [&](auto &l) {
						   auto *rLab = g.getReadLabel(l);
						   auto *rfLab = rLab->getRf();
						   return rLab->getAnnot() && // must be like that
							  rfLab->getStamp() > rLab->getStamp() &&
							  !getPrefixView(sLab).contains(
								  rfLab->getPos());
					   }),
			    loads.end());
	}

	return;
}

bool GenMCDriver::removeCASReadIfBlocks(const ReadLabel *rLab, const EventLabel *sLab)
{
	/* This only affects annotated CASes */
	if (!rLab->getAnnot() || !llvm::isa<CasReadLabel>(rLab) ||
	    (!getConf()->ipr && !llvm::isa<LockCasReadLabel>(rLab)))
		return false;
	/* Skip if bounding is enabled or the access is uninitialized */
	if (isUninitializedAccess(rLab->getAddr(), sLab->getPos()) || getConf()->bound.has_value())
		return false;

	/* If the CAS blocks, block thread altogether */
	auto val = getWriteValue(sLab, rLab->getAccess());
	if (rLab->valueMakesAssumeSucceed(val))
		return false;

	blockThread(ReadOptBlockLabel::create(rLab->getPos(), rLab->getAddr()));
	return true;
}

void GenMCDriver::checkReconsiderReadOpts(const WriteLabel *sLab)
{
	auto &g = getGraph();
	for (auto i = 0U; i < g.getNumThreads(); i++) {
		auto *bLab = llvm::dyn_cast<ReadOptBlockLabel>(g.getLastThreadLabel(i));
		if (!bLab || bLab->getAddr() != sLab->getAddr())
			continue;
		unblockThread(bLab->getPos());
	}
}

void GenMCDriver::optimizeUnconfirmedRevisits(const WriteLabel *sLab, std::vector<Event> &loads)
{
	if (!getConf()->helper)
		return;

	auto &g = getGraph();

	/* If there is already a write with the same value, report a possible ABA */
	auto valid = std::count_if(
		g.co_begin(sLab->getAddr()), g.co_end(sLab->getAddr()), [&](auto &wLab) {
			return wLab.getPos() != sLab->getPos() && wLab.getVal() == sLab->getVal();
		});
	if (sLab->getAddr().isStatic() &&
	    getWriteValue(g.getEventLabel(Event::getInit()), sLab->getAccess()) == sLab->getVal())
		++valid;
	WARN_ON_ONCE(valid > 0, "helper-aba-found",
		     "Possible ABA pattern! Consider running without -helper.\n");

	/* Do not bother with revisits that will be unconfirmed/lead to ABAs */
	loads.erase(std::remove_if(loads.begin(), loads.end(),
				   [&](const Event &l) {
					   auto *lab =
						   llvm::dyn_cast<ReadLabel>(g.getEventLabel(l));
					   if (!lab->isConfirming())
						   return false;

					   const EventLabel *scLab = nullptr;
					   auto *pLab = findMatchingSpeculativeRead(lab, scLab);
					   ERROR_ON(!pLab, "Confirming CAS annotation error! "
							   "Does a speculative read precede the "
							   "confirming operation?\n");

					   return !scLab;
				   }),
		    loads.end());
}

bool GenMCDriver::isConflictingNonRevBlocker(const EventLabel *pLab, const WriteLabel *sLab,
					     const Event &s)
{
	auto &g = getGraph();
	auto *sLab2 = llvm::dyn_cast<WriteLabel>(g.getEventLabel(s));
	if (sLab2->getPos() == sLab->getPos() || !sLab2->isRMW())
		return false;
	auto &prefix = getPrefixView(sLab);
	if (prefix.contains(sLab2->getPos()) && !(pLab && pLab->getStamp() < sLab2->getStamp()))
		return false;
	if (sLab2->getThread() <= sLab->getThread())
		return false;
	return std::any_of(sLab2->readers_begin(), sLab2->readers_end(), [&](auto &rLab) {
		return rLab.getStamp() < sLab2->getStamp() && !prefix.contains(rLab.getPos());
	});
}

bool GenMCDriver::tryOptimizeRevBlockerAddition(const WriteLabel *sLab, std::vector<Event> &loads)
{
	if (!sLab->hasAttr(WriteAttr::RevBlocker))
		return false;

	auto &g = getGraph();
	auto *pLab = getPreviousVisibleAccessLabel(sLab->getPos().prev());
	if (std::find_if(g.co_begin(sLab->getAddr()), g.co_end(sLab->getAddr()),
			 [this, pLab, sLab](auto &lab) {
				 return isConflictingNonRevBlocker(pLab, sLab, lab.getPos());
			 }) != g.co_end(sLab->getAddr())) {
		moot();
		loads.clear();
		return true;
	}
	return false;
}

bool GenMCDriver::tryOptimizeRevisits(WriteLabel *sLab, std::vector<Event> &loads)
{
	auto &g = getGraph();

	/* BAM */
	if (!getConf()->disableBAM) {
		if (auto *faiLab = llvm::dyn_cast<BIncFaiWriteLabel>(sLab)) {
			if (tryOptimizeBarrierRevisits(faiLab, loads))
				return true;
		}
	}

	/* IPR + locks */
	tryOptimizeIPRs(sLab, loads);

	/* Helper: 1) Do not bother with revisits that will lead to unconfirmed reads
		   2) Do not bother exploring if a RevBlocker is being re-added	*/
	if (getConf()->helper) {
		optimizeUnconfirmedRevisits(sLab, loads);
		if (sLab->hasAttr(WriteAttr::RevBlocker) &&
		    tryOptimizeRevBlockerAddition(sLab, loads))
			return true;
	}
	return false;
}

void GenMCDriver::revisitInPlace(const BackwardRevisit &br)
{
	BUG_ON(getConf()->bound.has_value());

	auto &g = getGraph();
	auto *rLab = g.getReadLabel(br.getPos());
	auto *sLab = g.getWriteLabel(br.getRev());

	BUG_ON(!llvm::isa<ReadLabel>(rLab));
	if (g.getNextLabel(rLab))
		g.removeLast(rLab->getThread());
	rLab->setRf(sLab);
	rLab->setAddedMax(true); // always true for atomicity violations
	rLab->setIPRStatus(true);

	completeRevisitedRMW(rLab);

	GENMC_DEBUG(LOG(VerbosityLevel::Debug1) << "--- In-place revisiting " << rLab->getPos()
						<< " <-- " << sLab->getPos() << "\n"
						<< getGraph(););

	EE->resetThread(rLab->getThread());
	EE->getThrById(rLab->getThread()).ECStack = EE->getThrById(rLab->getThread()).initEC;
	threadPrios = {rLab->getPos()};
}

void updatePredsWithPrefixView(const ExecutionGraph &g, VectorClock &preds,
			       const VectorClock &pporf)
{
	/* In addition to taking (preds U pporf), make sure pporf includes rfis */
	preds.update(pporf);

	if (!dynamic_cast<const DepExecutionGraph *>(&g))
		return;
	auto &predsD = *llvm::dyn_cast<DepView>(&preds);
	for (auto i = 0u; i < pporf.size(); i++) {
		for (auto j = 1; j <= pporf.getMax(i); j++) {
			auto *lab = g.getEventLabel(Event(i, j));
			if (auto *rLab = llvm::dyn_cast<ReadLabel>(lab)) {
				if (preds.contains(rLab->getPos()) &&
				    !preds.contains(rLab->getRf())) {
					if (rLab->getRf()->getThread() == rLab->getThread())
						predsD.removeHole(rLab->getRf()->getPos());
				}
			}
			auto *wLab = llvm::dyn_cast<WriteLabel>(lab);
			if (wLab && wLab->isRMW() && pporf.contains(lab->getPos().prev()))
				predsD.removeHole(lab->getPos());
		}
	}
	return;
}

std::unique_ptr<VectorClock>
GenMCDriver::getRevisitView(const ReadLabel *rLab, const WriteLabel *sLab,
			    const WriteLabel *midLab /* = nullptr */) const
{
	auto &g = getGraph();
	auto preds = g.getPredsView(rLab->getPos());

	updatePredsWithPrefixView(g, *preds, getPrefixView(sLab));
	if (midLab)
		updatePredsWithPrefixView(g, *preds, getPrefixView(midLab));
	return std::move(preds);
}

std::unique_ptr<BackwardRevisit> GenMCDriver::constructBackwardRevisit(const ReadLabel *rLab,
								       const WriteLabel *sLab) const
{
	if (!getConf()->helper)
		return std::make_unique<BackwardRevisit>(rLab, sLab, getRevisitView(rLab, sLab));

	auto &g = getGraph();

	/* Check whether there is a conflicting RevBlocker */
	auto pending = g.getPendingRMW(sLab);
	auto *pLab = llvm::dyn_cast_or_null<WriteLabel>(g.getNextLabel(g.getEventLabel(pending)));
	pending = (!pending.isInitializer() && pLab->hasAttr(WriteAttr::RevBlocker))
			  ? pending.next()
			  : Event::getInit();

	/* If there is, do an optimized backward revisit */
	auto &prefix = getPrefixView(sLab);
	if (!pending.isInitializer() &&
	    !getPrefixView(g.getEventLabel(pending)).contains(rLab->getPos()) &&
	    rLab->getStamp() < g.getEventLabel(pending)->getStamp() && !prefix.contains(pending))
		return std::make_unique<BackwardRevisitHELPER>(
			rLab->getPos(), sLab->getPos(),
			getRevisitView(rLab, sLab, g.getWriteLabel(pending)), pending);
	return std::make_unique<BackwardRevisit>(rLab, sLab, getRevisitView(rLab, sLab));
}

bool isFixedHoleInView(const ExecutionGraph &g, const EventLabel *lab, const DepView &v)
{
	if (auto *wLabB = llvm::dyn_cast<WriteLabel>(lab))
		return std::any_of(wLabB->readers_begin(), wLabB->readers_end(),
				   [&v](auto &oLab) { return v.contains(oLab.getPos()); });

	auto *rLabB = llvm::dyn_cast<ReadLabel>(lab);
	if (!rLabB)
		return false;

	/* If prefix has same address load, we must read from the same write */
	for (auto i = 0u; i < v.size(); i++) {
		for (auto j = 0u; j <= v.getMax(i); j++) {
			if (!v.contains(Event(i, j)))
				continue;
			if (auto *mLab = g.getReadLabel(Event(i, j)))
				if (mLab->getAddr() == rLabB->getAddr() &&
				    mLab->getRf() == rLabB->getRf())
					return true;
		}
	}

	if (rLabB->isRMW()) {
		auto *wLabB = g.getWriteLabel(rLabB->getPos().next());
		return std::any_of(wLabB->readers_begin(), wLabB->readers_end(),
				   [&v](auto &oLab) { return v.contains(oLab.getPos()); });
	}
	return false;
}

bool GenMCDriver::prefixContainsSameLoc(const BackwardRevisit &r, const EventLabel *lab) const
{
	if (!getConf()->isDepTrackingModel)
		return false;

	/* Some holes need to be treated specially. However, it is _wrong_ to keep
	 * porf views around. What we should do instead is simply check whether
	 * an event is "part" of WLAB's pporf view (even if it is not contained in it).
	 * Similar actions are taken in {WB,MO}Calculator */
	auto &g = getGraph();
	auto &v = *llvm::dyn_cast<DepView>(&getPrefixView(g.getEventLabel(r.getRev())));
	if (lab->getIndex() <= v.getMax(lab->getThread()) && isFixedHoleInView(g, lab, v))
		return true;
	if (auto *br = llvm::dyn_cast<BackwardRevisitHELPER>(&r)) {
		auto &hv = *llvm::dyn_cast<DepView>(&getPrefixView(g.getEventLabel(br->getMid())));
		return lab->getIndex() <= hv.getMax(lab->getThread()) &&
		       isFixedHoleInView(g, lab, hv);
	}
	return false;
}

bool GenMCDriver::hasBeenRevisitedByDeleted(const BackwardRevisit &r, const EventLabel *eLab)
{
	auto *lab = llvm::dyn_cast<ReadLabel>(eLab);
	if (!lab || lab->isIPR())
		return false;

	auto *rfLab = lab->getRf();
	auto &v = *r.getViewNoRel();
	return !v.contains(rfLab->getPos()) && rfLab->getStamp() > lab->getStamp() &&
	       !prefixContainsSameLoc(r, rfLab);
}

bool GenMCDriver::isCoBeforeSavedPrefix(const BackwardRevisit &r, const EventLabel *lab)
{
	auto *mLab = llvm::dyn_cast<MemAccessLabel>(lab);
	if (!mLab)
		return false;

	auto &g = getGraph();
	auto &v = r.getViewNoRel();
	auto w = llvm::isa<ReadLabel>(mLab) ? llvm::dyn_cast<ReadLabel>(mLab)->getRf()->getPos()
					    : mLab->getPos();
	auto succIt = g.getWriteLabel(w) ? g.co_succ_begin(g.getWriteLabel(w))
					 : g.co_begin(mLab->getAddr());
	auto succE = g.getWriteLabel(w) ? g.co_succ_end(g.getWriteLabel(w))
					: g.co_end(mLab->getAddr());
	return any_of(succIt, succE, [&](auto &sLab) {
		return v->contains(sLab.getPos()) &&
		       (!getConf()->isDepTrackingModel ||
			mLab->getIndex() > getPrefixView(&sLab).getMax(mLab->getThread())) &&
		       sLab.getPos() != r.getRev();
	});
}

bool GenMCDriver::coherenceSuccRemainInGraph(const BackwardRevisit &r)
{
	auto &g = getGraph();
	auto *wLab = g.getWriteLabel(r.getRev());
	if (wLab->isRMW())
		return true;

	auto succIt = g.co_succ_begin(wLab);
	auto succE = g.co_succ_end(wLab);
	if (succIt == succE)
		return true;

	return r.getViewNoRel()->contains(succIt->getPos());
}

bool wasAddedMaximally(const EventLabel *lab)
{
	if (auto *mLab = llvm::dyn_cast<MemAccessLabel>(lab))
		return mLab->wasAddedMax();
	if (auto *oLab = llvm::dyn_cast<OptionalLabel>(lab))
		return !oLab->isExpanded();
	return true;
}

bool GenMCDriver::isMaximalExtension(const BackwardRevisit &r)
{
	if (!coherenceSuccRemainInGraph(r))
		return false;

	auto &g = getGraph();
	auto &v = r.getViewNoRel();

	for (const auto &lab : g.labels()) {
		if ((lab.getPos() != r.getPos() && v->contains(lab.getPos())) ||
		    prefixContainsSameLoc(r, &lab))
			continue;

		if (!wasAddedMaximally(&lab))
			return false;
		if (isCoBeforeSavedPrefix(r, &lab))
			return false;
		if (hasBeenRevisitedByDeleted(r, &lab))
			return false;
	}
	return true;
}

bool GenMCDriver::revisitModifiesGraph(const BackwardRevisit &r) const
{
	auto &g = getGraph();
	auto &v = r.getViewNoRel();
	for (auto i = 0u; i < g.getNumThreads(); i++) {
		if (v->getMax(i) + 1 != (long)g.getThreadSize(i) &&
		    !llvm::isa<TerminatorLabel>(g.getEventLabel(Event(i, v->getMax(i) + 1))))
			return true;
		if (!getConf()->isDepTrackingModel)
			continue;
		for (auto j = 0u; j < g.getThreadSize(i); j++) {
			auto *lab = g.getEventLabel(Event(i, j));
			if (!v->contains(lab->getPos()) && !llvm::isa<EmptyLabel>(lab) &&
			    !llvm::isa<TerminatorLabel>(lab))
				return true;
		}
	}
	return false;
}

std::unique_ptr<ExecutionGraph> GenMCDriver::copyGraph(const BackwardRevisit *br,
						       VectorClock *v) const
{
	auto &g = getGraph();

	/* Adjust the view that will be used for copying */
	auto &prefix = getPrefixView(g.getEventLabel(br->getRev()));
	if (auto *brh = llvm::dyn_cast<BackwardRevisitHELPER>(br)) {
		if (auto *dv = llvm::dyn_cast<DepView>(v)) {
			dv->addHole(brh->getMid());
			dv->addHole(brh->getMid().prev());
		} else {
			auto prev = v->getMax(brh->getMid().thread);
			v->setMax(Event(brh->getMid().thread, prev - 2));
		}
	}

	auto og = g.getCopyUpTo(*v);

	/* Ensure the prefix of the write will not be revisitable */
	auto *revLab = og->getReadLabel(br->getPos());

	og->compressStampsAfter(revLab->getStamp());
	for (auto &lab : og->labels()) {
		if (prefix.contains(lab.getPos()))
			lab.setRevisitStatus(false);
	}
	return og;
}

GenMCDriver::ChoiceMap GenMCDriver::createChoiceMapForCopy(const ExecutionGraph &og) const
{
	const auto &g = getGraph();
	const auto &choices = getChoiceMap();
	ChoiceMap result;

	for (auto &lab : g.labels()) {
		if (!og.containsPos(lab.getPos()) || !choices.count(lab.getStamp()))
			continue;

		auto oldStamp = lab.getStamp();
		auto newStamp = og.getEventLabel(lab.getPos())->getStamp();
		for (const auto &[s, w] : choices.at(oldStamp)) {
			if (og.containsPos(s))
				result[newStamp.get()].insert({s, w});
		}
	}
	return result;
}

bool GenMCDriver::checkRevBlockHELPER(const WriteLabel *sLab, const std::vector<Event> &loads)
{
	if (!getConf()->helper || !sLab->hasAttr(WriteAttr::RevBlocker))
		return true;

	auto &g = getGraph();
	if (std::any_of(loads.begin(), loads.end(), [this, &g, sLab](const Event &l) {
		    auto *lLab = g.getLastThreadLabel(l.thread);
		    auto *pLab = getPreviousVisibleAccessLabel(lLab->getPos());
		    return llvm::isa<BlockLabel>(lLab) && pLab && pLab->getPos() == l;
	    })) {
		moot();
		return false;
	}
	return true;
}

void GenMCDriver::updateStSpaceChoices(const std::vector<Event> &loads, const WriteLabel *sLab)
{
	auto &g = getGraph();
	auto &choices = getChoiceMap();

	for (const auto &l : loads) {
		const auto *rLab = g.getReadLabel(l);
		choices[rLab->getStamp()].insert(std::make_pair(sLab->getPos(), -1));
	}
}

bool GenMCDriver::calcRevisits(WriteLabel *sLab)
{
	auto &g = getGraph();
	auto loads = getRevisitableApproximation(sLab);

	GENMC_DEBUG(LOG(VerbosityLevel::Debug3) << "Revisitable: " << format(loads) << "\n";);
	if (tryOptimizeRevisits(sLab, loads))
		return true;
	/* If operating in estimation mode, don't actually revisit */
	if (inEstimationMode()) {
		updateStSpaceChoices(loads, sLab);
		return checkAtomicity(sLab) && checkRevBlockHELPER(sLab, loads) && !isMoot();
	}
	if (inFuzzingMode()) {
		for (auto &l : loads) {
			auto [vals, last] = extractValPrefix(l);

			vals.push_back(getWriteValue(sLab));
			auto *data = seenValues[l.thread].lookup(vals);
			if (!data) {
				seenValues[l.thread].addSeq(vals, 0);
			}
		}

		std::vector<Event> ls;
		Event lastAddedCache = lastAdded;

		std::uniform_real_distribution<> dis(0.0, 1.0);
		for (auto &l : loads) {
			if (getConf()->mutationBound &&
			    dis(rng) >= (double)(*getConf()->mutationBound) / loads.size())
				continue;
			auto &g = getGraph();
			auto *rLab = g.getReadLabel(l);
			auto v = getRevisitView(rLab, sLab);
			auto br = constructBackwardRevisit(rLab, sLab);
			auto og = g.getCopyUpTo(*v);
			og->compressStampsAfter(rLab->getStamp());

			pushExecution({std::move(og), LocalQueueT(), {}});
			repairDanglingReads(getGraph());
			auto ok = revisitRead(*br);

			if (ok && isRevisitValid(*br))
				ls.push_back(l);
			popExecution();
		}
		lastAdded = lastAddedCache;
		updateStSpaceChoices(ls, sLab);
		return checkAtomicity(sLab) && checkRevBlockHELPER(sLab, ls) && !isMoot();
	}

	GENMC_DEBUG(LOG(VerbosityLevel::Debug3)
			    << "Revisitable (optimized): " << format(loads) << "\n";);
	for (auto &l : loads) {
		auto *rLab = g.getReadLabel(l);
		BUG_ON(!rLab);

		auto br = constructBackwardRevisit(rLab, sLab);
		if (!isMaximalExtension(*br))
			break;

		addToWorklist(sLab->getStamp(), std::move(br));
	}

	return checkAtomicity(sLab) && checkRevBlockHELPER(sLab, loads) && !isMoot();
}

WriteLabel *GenMCDriver::completeRevisitedRMW(const ReadLabel *rLab)
{
	/* Handle non-RMW cases first */
	if (!llvm::isa<CasReadLabel>(rLab) && !llvm::isa<FaiReadLabel>(rLab))
		return nullptr;
	if (auto *casLab = llvm::dyn_cast<CasReadLabel>(rLab)) {
		if (getReadValue(rLab) != casLab->getExpected())
			return nullptr;
	}

	SVal result;
	WriteAttr wattr = WriteAttr::None;
	if (auto *faiLab = llvm::dyn_cast<FaiReadLabel>(rLab)) {
		/* Need to get the rf value within the if, as rLab might be a disk op,
		 * and we cannot get the value in that case (but it will also not be an RMW)
		 */
		auto rfVal = getReadValue(rLab);
		result = getEE()->executeAtomicRMWOperation(rfVal, faiLab->getOpVal(),
							    faiLab->getSize(), faiLab->getOp());
		if (llvm::isa<BIncFaiReadLabel>(faiLab) && result == SVal(0))
			result = getBarrierInitValue(rLab->getAccess());
		wattr = faiLab->getAttr();
	} else if (auto *casLab = llvm::dyn_cast<CasReadLabel>(rLab)) {
		result = casLab->getSwapVal();
		wattr = casLab->getAttr();
	} else
		BUG();

	auto &g = getGraph();
	std::unique_ptr<WriteLabel> wLab = nullptr;

#define CREATE_COUNTERPART(name)                                                                   \
	case EventLabel::name##Read:                                                               \
		wLab = name##WriteLabel::create(rLab->getPos().next(), rLab->getOrdering(),        \
						rLab->getAddr(), rLab->getSize(), rLab->getType(), \
						result, wattr);                                    \
		break;

	switch (rLab->getKind()) {
		CREATE_COUNTERPART(BIncFai);
		CREATE_COUNTERPART(NoRetFai);
		CREATE_COUNTERPART(Fai);
		CREATE_COUNTERPART(LockCas);
		CREATE_COUNTERPART(TrylockCas);
		CREATE_COUNTERPART(Cas);
		CREATE_COUNTERPART(HelpedCas);
		CREATE_COUNTERPART(ConfirmingCas);
	default:
		BUG();
	}
	BUG_ON(!wLab);
	auto *lab = llvm::dyn_cast<WriteLabel>(addLabelToGraph(std::move(wLab)));
	BUG_ON(!rLab->getRf());
	g.addStoreToCOAfter(lab, rLab->getRf());
	return lab;
}

bool GenMCDriver::revisitWrite(const WriteForwardRevisit &ri)
{
	auto &g = getGraph();
	auto *wLab = g.getWriteLabel(ri.getPos());
	BUG_ON(!wLab);

	g.moveStoreCOAfter(wLab, g.getEventLabel(ri.getPred()));
	wLab->setAddedMax(false);
	return calcRevisits(wLab);
}

bool GenMCDriver::revisitOptional(const OptionalForwardRevisit &oi)
{
	auto &g = getGraph();
	auto *oLab = llvm::dyn_cast<OptionalLabel>(g.getEventLabel(oi.getPos()));

	--result.exploredBlocked;
	BUG_ON(!oLab);
	oLab->setExpandable(false);
	oLab->setExpanded(true);
	return true;
}

bool GenMCDriver::revisitRead(const Revisit &ri)
{
	BUG_ON(!llvm::isa<ReadRevisit>(&ri));

	/* We are dealing with a read: change its reads-from and also check
	 * whether a part of an RMW should be added */
	auto &g = getGraph();
	auto *rLab = g.getReadLabel(ri.getPos());
	auto *revLab = g.getEventLabel(llvm::dyn_cast<ReadRevisit>(&ri)->getRev());

	rLab->setRf(revLab);
	auto *fri = llvm::dyn_cast<ReadForwardRevisit>(&ri);
	rLab->setAddedMax(fri ? fri->isMaximal() : revLab == g.co_max(rLab->getAddr()));
	rLab->setIPRStatus(false);

	GENMC_DEBUG(LOG(VerbosityLevel::Debug1)
			    << "--- " << (llvm::isa<BackwardRevisit>(ri) ? "Backward" : "Forward")
			    << " revisiting " << ri.getPos() << " <-- " << revLab->getPos() << "\n"
			    << getGraph(););

	/*  Try to remove the read from the execution */
	if (removeCASReadIfBlocks(rLab, revLab))
		return true;

	/* If the revisited label became an RMW, add the store part and revisit */
	if (auto *sLab = completeRevisitedRMW(rLab)) {
		auto ok = calcRevisits(sLab);
		return ok;
	}

	/* Blocked barrier: block thread */
	if (llvm::isa<BWaitReadLabel>(rLab) &&
	    getReadValue(rLab) != getBarrierInitValue(rLab->getAccess())) {
		blockThread(BarrierBlockLabel::create(rLab->getPos().next()));
	}

	/* Blocked lock -> prioritize locking thread */
	if (llvm::isa<LockCasReadLabel>(rLab)) {
		blockThread(LockNotAcqBlockLabel::create(rLab->getPos().next()));
		if (!getConf()->bound.has_value())
			threadPrios = {rLab->getRf()->getPos()};
	}
	auto rpreds = po_preds(g, rLab);
	auto oLabIt = std::ranges::find_if(
		rpreds, [&](auto &oLab) { return llvm::isa<SpeculativeReadLabel>(&oLab); });
	if (getConf()->helper && (llvm::isa<SpeculativeReadLabel>(rLab) || oLabIt != rpreds.end()))
		threadPrios = {rLab->getPos()};
	return true;
}

bool GenMCDriver::forwardRevisit(const ForwardRevisit &fr)
{
	auto &g = getGraph();
	auto *lab = g.getEventLabel(fr.getPos());
	if (auto *mi = llvm::dyn_cast<WriteForwardRevisit>(&fr))
		return revisitWrite(*mi);
	if (auto *oi = llvm::dyn_cast<OptionalForwardRevisit>(&fr))
		return revisitOptional(*oi);
	if (auto *rr = llvm::dyn_cast<RerunForwardRevisit>(&fr))
		return true;
	auto *ri = llvm::dyn_cast<ReadForwardRevisit>(&fr);
	BUG_ON(!ri);
	return revisitRead(*ri);
}

bool GenMCDriver::backwardRevisit(const BackwardRevisit &br)
{
	auto &g = getGraph();

	/* Recalculate the view because some B labels might have been
	 * removed */
	auto *brh = llvm::dyn_cast<BackwardRevisitHELPER>(&br);
	auto v = getRevisitView(g.getReadLabel(br.getPos()), g.getWriteLabel(br.getRev()),
				brh ? g.getWriteLabel(brh->getMid()) : nullptr);

	auto og = copyGraph(&br, &*v);
	auto m = createChoiceMapForCopy(*og);

	pushExecution({std::move(og), LocalQueueT(), std::move(m)});

	repairDanglingReads(getGraph());
	auto ok = revisitRead(br);
	BUG_ON(!ok);

	/* If there are idle workers in the thread pool,
	 * try submitting the job instead */
	auto *tp = getThreadPool();
	if (tp && tp->getRemainingTasks() < 8 * tp->size()) {
		if (isRevisitValid(br))
			tp->submit(extractState());
		return false;
	}
	return true;
}

bool GenMCDriver::restrictAndRevisit(Stamp stamp, const WorkSet::ItemT &item)
{
	/* First, appropriately restrict the worklist and the graph */
	getExecution().restrict(stamp);

	lastAdded = item->getPos();
	if (auto *fr = llvm::dyn_cast<ForwardRevisit>(&*item))
		return forwardRevisit(*fr);
	if (auto *br = llvm::dyn_cast<BackwardRevisit>(&*item)) {
		return backwardRevisit(*br);
	}
	BUG();
	return false;
}

bool GenMCDriver::handleHelpingCas(std::unique_ptr<HelpingCasLabel> hLab)
{
	if (isExecutionDrivenByGraph(&*hLab))
		return true;

	/* Ensure that the helped CAS exists */
	auto *lab = llvm::dyn_cast<HelpingCasLabel>(addLabelToGraph(std::move(hLab)));
	if (!checkHelpingCasCondition(lab)) {
		blockThread(HelpedCASBlockLabel::create(lab->getPos()));
		return false;
	}
	return true;
}

bool GenMCDriver::handleOptional(std::unique_ptr<OptionalLabel> lab)
{
	auto &g = getGraph();

	if (isExecutionDrivenByGraph(&*lab))
		return llvm::dyn_cast<OptionalLabel>(g.getEventLabel(lab->getPos()))->isExpanded();

	if (std::any_of(g.label_begin(), g.label_end(), [&](auto &lab) {
		    auto *oLab = llvm::dyn_cast<OptionalLabel>(&lab);
		    return oLab && !oLab->isExpandable();
	    }))
		lab->setExpandable(false);

	auto *oLab = llvm::dyn_cast<OptionalLabel>(addLabelToGraph(std::move(lab)));

	if (inVerificationMode() && oLab->isExpandable())
		addToWorklist(oLab->getStamp(),
			      std::make_unique<OptionalForwardRevisit>(oLab->getPos()));
	return false; /* should not be expanded yet */
}

bool GenMCDriver::isWriteEffectful(const WriteLabel *wLab)
{
	auto &g = getGraph();
	auto *xLab = llvm::dyn_cast<FaiWriteLabel>(wLab);
	auto *rLab = llvm::dyn_cast<FaiReadLabel>(g.getPreviousLabel(wLab));
	if (!xLab || rLab->getOp() != llvm::AtomicRMWInst::BinOp::Xchg)
		return true;

	return getReadValue(rLab) != xLab->getVal();
}

bool GenMCDriver::isWriteObservable(const WriteLabel *wLab)
{
	if (wLab->isAtLeastRelease() || !wLab->getAddr().isDynamic())
		return true;

	auto &g = getGraph();
	auto wpreds = po_preds(g, wLab);
	auto mLabIt = std::ranges::find_if(wpreds, [wLab](auto &lab) {
		if (auto *aLab = llvm::dyn_cast<MallocLabel>(&lab)) {
			if (aLab->contains(wLab->getAddr()))
				return true;
		}
		return false;
	});
	if (mLabIt == std::ranges::end(wpreds))
		return true;

	auto *mLab = &*mLabIt;
	for (auto j = mLab->getIndex() + 1; j < wLab->getIndex(); j++) {
		auto *lab = g.getEventLabel(Event(wLab->getThread(), j));
		if (lab->isAtLeastRelease())
			return true;
		/* The location must not be read (loop counter) */
		if (auto *rLab = llvm::dyn_cast<ReadLabel>(lab))
			if (rLab->getAddr() == wLab->getAddr())
				return true;
	}
	return false;
}

void GenMCDriver::handleSpinStart(std::unique_ptr<SpinStartLabel> lab)
{
	auto &g = getGraph();

	/* If it has not been added to the graph, do so */
	if (isExecutionDrivenByGraph(&*lab))
		return;

	auto *stLab = addLabelToGraph(std::move(lab));

	/* Check whether we can detect some spinloop dynamically */
	auto stpreds = po_preds(g, stLab);
	auto lbLabIt = std::ranges::find_if(
		stpreds, [](auto &lab) { return llvm::isa<LoopBeginLabel>(lab); });

	/* If we did not find a loop-begin, this a manual instrumentation(?); report to user
	 */
	ERROR_ON(lbLabIt == stpreds.end(), "No loop-beginning found!\n");

	auto *lbLab = &*lbLabIt;
	auto pLabIt = std::ranges::find_if(stpreds, [lbLab](auto &lab) {
		return llvm::isa<SpinStartLabel>(&lab) && lab.getIndex() > lbLab->getIndex();
	});
	if (pLabIt == stpreds.end())
		return;

	auto *pLab = &*pLabIt;
	for (auto i = pLab->getIndex() + 1; i < stLab->getIndex(); i++) {
		auto *wLab =
			llvm::dyn_cast<WriteLabel>(g.getEventLabel(Event(stLab->getThread(), i)));
		if (wLab && isWriteEffectful(wLab) && isWriteObservable(wLab))
			return; /* found event w/ side-effects */
	}
	/* Spinloop detected */
	blockThreadTryMoot(SpinloopBlockLabel::create(stLab->getPos()));
}

bool GenMCDriver::areFaiZNEConstraintsSat(const FaiZNESpinEndLabel *lab)
{
	auto &g = getGraph();

	/* Check that there are no other side-effects since the previous iteration.
	 * We don't have to look for a BEGIN label since ZNE labels are always
	 * preceded by a spin-start */
	auto preds = po_preds(g, lab);
	auto ssLabIt = std::ranges::find_if(
		preds, [](auto &lab) { return llvm::isa<SpinStartLabel>(&lab); });
	BUG_ON(ssLabIt == preds.end());
	auto *ssLab = &*ssLabIt;
	for (auto i = ssLab->getIndex() + 1; i < lab->getIndex(); ++i) {
		auto *oLab = g.getEventLabel(Event(ssLab->getThread(), i));
		if (llvm::isa<WriteLabel>(oLab) && !llvm::isa<FaiWriteLabel>(oLab))
			return false;
	}

	auto wLabIt = std::ranges::find_if(
		preds, [](auto &lab) { return llvm::isa<FaiWriteLabel>(&lab); });
	BUG_ON(wLabIt == preds.end());

	/* All stores in the RMW chain need to be read from at most 1 read,
	 * and there need to be no other stores that are not hb-before lab */
	auto *wLab = llvm::dyn_cast<FaiWriteLabel>(&*wLabIt);
	for (auto &lab : g.labels()) {
		if (auto *mLab = llvm::dyn_cast<MemAccessLabel>(&lab)) {
			if (mLab->getAddr() == wLab->getAddr() && !llvm::isa<FaiReadLabel>(mLab) &&
			    !llvm::isa<FaiWriteLabel>(mLab) &&
			    !getConsChecker().getHbView(wLab).contains(mLab->getPos()))
				return false;
		}
	}
	return true;
}

void GenMCDriver::handleFaiZNESpinEnd(std::unique_ptr<FaiZNESpinEndLabel> lab)
{
	auto &g = getGraph();

	/* If we are actually replaying this one, it is not a spin loop*/
	if (isExecutionDrivenByGraph(&*lab))
		return;

	auto *zLab = llvm::dyn_cast<FaiZNESpinEndLabel>(addLabelToGraph(std::move(lab)));
	if (areFaiZNEConstraintsSat(zLab))
		blockThreadTryMoot(FaiZNEBlockLabel::create(zLab->getPos()));
}

void GenMCDriver::handleLockZNESpinEnd(std::unique_ptr<LockZNESpinEndLabel> lab)
{
	if (isExecutionDrivenByGraph(&*lab))
		return;

	auto *zLab = addLabelToGraph(std::move(lab));
	blockThreadTryMoot(LockZNEBlockLabel::create(zLab->getPos()));
}

void GenMCDriver::handleDummy(std::unique_ptr<EventLabel> lab)
{
	if (!isExecutionDrivenByGraph(&*lab))
		addLabelToGraph(std::move(lab));
}

/************************************************************
 ** Printing facilities
 ***********************************************************/

static void executeMDPrint(const EventLabel *lab, const std::pair<int, std::string> &locAndFile,
			   std::string inputFile, llvm::raw_ostream &os = llvm::outs())
{
	std::string errPath = locAndFile.second;
	Parser::stripSlashes(errPath);
	Parser::stripSlashes(inputFile);

	os << " ";
	if (errPath != inputFile)
		os << errPath << ":";
	else
		os << "L.";
	os << locAndFile.first;
}

/* Returns true if the corresponding LOC should be printed for this label type */
bool shouldPrintLOC(const EventLabel *lab)
{
	/* Begin/End labels don't have a corresponding LOC */
	if (llvm::isa<ThreadStartLabel>(lab) || llvm::isa<ThreadFinishLabel>(lab))
		return false;

	/* Similarly for allocations that don't come from malloc() */
	if (auto *mLab = llvm::dyn_cast<MallocLabel>(lab))
		return mLab->getAllocAddr().isHeap() && !mLab->getAllocAddr().isInternal();

	return true;
}

std::string GenMCDriver::getVarName(const SAddr &addr) const
{
	if (addr.isStatic())
		return getEE()->getStaticName(addr);

	auto &g = getGraph();
	auto *aLab = findAllocatingLabel(g, addr);

	if (!aLab)
		return "???";
	if (aLab->getNameInfo())
		return aLab->getName() +
		       aLab->getNameInfo()->getNameAtOffset(addr - aLab->getAllocAddr());
	return "";
}

#ifdef ENABLE_GENMC_DEBUG
llvm::raw_ostream::Colors getLabelColor(const EventLabel *lab)
{
	auto *mLab = llvm::dyn_cast<MemAccessLabel>(lab);
	if (!mLab)
		return llvm::raw_ostream::Colors::WHITE;

	if (llvm::isa<ReadLabel>(mLab) && !llvm::dyn_cast<ReadLabel>(mLab)->isRevisitable())
		return llvm::raw_ostream::Colors::RED;
	if (mLab->wasAddedMax())
		return llvm::raw_ostream::Colors::GREEN;
	return llvm::raw_ostream::Colors::WHITE;
}
#endif

void GenMCDriver::printGraph(bool printMetadata /* false */,
			     llvm::raw_ostream &s /* = llvm::dbgs() */)
{
	auto &g = getGraph();
	LabelPrinter printer([this](const SAddr &saddr) { return getVarName(saddr); },
			     [this](const ReadLabel &lab) { return getReadValue(&lab); });

	/* Print the graph */
	for (auto i = 0u; i < g.getNumThreads(); i++) {
		auto &thr = EE->getThrById(i);
		s << thr;
		if (getConf()->symmetryReduction) {
			if (auto *bLab = g.getFirstThreadLabel(i)) {
				auto symm = bLab->getSymmetricTid();
				if (symm != -1)
					s << " symmetric with " << symm;
			}
		}
		s << ":\n";
		for (auto j = 1u; j < g.getThreadSize(i); j++) {
			auto *lab = g.getEventLabel(Event(i, j));
			s << "\t";
			GENMC_DEBUG(if (getConf()->colorAccesses)
					    s.changeColor(getLabelColor(lab)););
			s << printer.toString(*lab);
			GENMC_DEBUG(s.resetColor(););
			GENMC_DEBUG(if (getConf()->printStamps) s << " @ " << lab->getStamp(););
			if (printMetadata && thr.prefixLOC[j].first && shouldPrintLOC(lab)) {
				executeMDPrint(lab, thr.prefixLOC[j], getConf()->inputFile, s);
			}
			s << "\n";
		}
	}

	/* MO: Print coherence information */
	auto header = false;
	for (auto locIt = g.loc_begin(), locE = g.loc_end(); locIt != locE; ++locIt) {
		/* Skip empty and single-store locations */
		if (g.hasLocMoreThanOneStore(locIt->first)) {
			if (!header) {
				s << "Coherence:\n";
				header = true;
			}
			auto *wLab = &*g.co_begin(locIt->first);
			s << getVarName(wLab->getAddr()) << ": [ ";
			for (const auto &w : g.co(locIt->first))
				s << w << " ";
			s << "]\n";
		}
	}
	s << "\n";
}

void GenMCDriver::dotPrintToFile(const std::string &filename, const EventLabel *errLab,
				 const EventLabel *confLab)
{
	auto &g = getGraph();
	auto *EE = getEE();
	std::ofstream fout(filename);
	llvm::raw_os_ostream ss(fout);
	DotPrinter printer([this](const SAddr &saddr) { return getVarName(saddr); },
			   [this](const ReadLabel &lab) { return getReadValue(&lab); });

	auto before = getPrefixView(errLab).clone();
	if (confLab)
		before->update(getPrefixView(confLab));

	/* Create a directed graph graph */
	ss << "strict digraph {\n";
	/* Specify node shape */
	ss << "node [shape=plaintext]\n";
	/* Left-justify labels for clusters */
	ss << "labeljust=l\n";
	/* Draw straight lines */
	ss << "splines=false\n";

	/* Print all nodes with each thread represented by a cluster */
	for (auto i = 0u; i < before->size(); i++) {
		auto &thr = EE->getThrById(i);
		ss << "subgraph cluster_" << thr.id << "{\n";
		ss << "\tlabel=\"" << thr.threadFun->getName().str() << "()\"\n";
		for (auto j = 1; j <= before->getMax(i); j++) {
			auto *lab = g.getEventLabel(Event(i, j));

			ss << "\t\"" << lab->getPos() << "\" [label=<";

			/* First, print the graph label for this node */
			ss << printer.toString(*lab);

			/* And then, print the corresponding line number */
			if (!thr.prefixLOC.empty() && thr.prefixLOC[j].first &&
			    shouldPrintLOC(lab)) {
				ss << " <FONT COLOR=\"gray\">";
				executeMDPrint(lab, thr.prefixLOC[j], getConf()->inputFile, ss);
				ss << "</FONT>";
			}

			ss << ">"
			   << (lab->getPos() == errLab->getPos() ||
					       (confLab && lab->getPos() == confLab->getPos())
				       ? ",style=filled,fillcolor=yellow"
				       : "")
			   << "]\n";
		}
		ss << "}\n";
	}

	/* Print relations between events (po U rf) */
	for (auto i = 0u; i < before->size(); i++) {
		auto &thr = EE->getThrById(i);
		for (auto j = 0; j <= before->getMax(i); j++) {
			auto *lab = g.getEventLabel(Event(i, j));

			/* Print a po-edge, but skip dummy start events for
			 * all threads except for the first one */
			if (j < before->getMax(i) && !llvm::isa<ThreadStartLabel>(lab))
				ss << "\"" << lab->getPos() << "\" -> \"" << lab->getPos().next()
				   << "\"\n";
			if (auto *rLab = llvm::dyn_cast<ReadLabel>(lab)) {
				/* Do not print RFs from INIT, BOTTOM, and same thread */
				if (llvm::dyn_cast_or_null<WriteLabel>(rLab->getRf()) &&
				    rLab->getRf()->getThread() != lab->getThread()) {
					ss << "\"" << rLab->getRf()->getPos() << "\" -> \""
					   << rLab->getPos()
					   << "\"[color=green, constraint=false]\n";

					// if (auto &choices = getChoiceMap()[rLab->getStamp()];
					//     choices.size() > 1) {
					// 	for (auto &&[e, _] : choices) {
					// 		if (e != rLab->getRf()->getPos()) {
					// 			ss << "\"" << e << "\" -> \""
					// 			   << rLab->getPos()
					// 			   << "\"[color=gray, "
					// 			      "constraint=false]\n";
					// 		}
					// 	}
					// }
				}
			}
			if (auto *bLab = llvm::dyn_cast<ThreadStartLabel>(lab)) {
				if (thr.id == 0)
					continue;
				ss << "\"" << bLab->getParentCreate() << "\" -> \""
				   << bLab->getPos().next() << "\"[color=blue, constraint=false]\n";
			}
			if (auto *jLab = llvm::dyn_cast<ThreadJoinLabel>(lab))
				ss << "\"" << g.getLastThreadLabel(jLab->getChildId())->getPos()
				   << "\" -> \"" << jLab->getPos()
				   << "\"[color=blue, constraint=false]\n";
		}
	}

	ss << "}\n";
}

void GenMCDriver::recPrintTraceBefore(const Event &e, View &a,
				      llvm::raw_ostream &ss /* llvm::outs() */)
{
	const auto &g = getGraph();

	if (a.contains(e))
		return;

	auto ai = a.getMax(e.thread);
	a.setMax(e);
	auto &thr = getEE()->getThrById(e.thread);
	for (int i = ai; i <= e.index; i++) {
		const EventLabel *lab = g.getEventLabel(Event(e.thread, i));
		if (auto *rLab = llvm::dyn_cast<ReadLabel>(lab))
			if (rLab->getRf())
				recPrintTraceBefore(rLab->getRf()->getPos(), a, ss);
		if (auto *jLab = llvm::dyn_cast<ThreadJoinLabel>(lab))
			recPrintTraceBefore(g.getLastThreadLabel(jLab->getChildId())->getPos(), a,
					    ss);
		if (auto *bLab = llvm::dyn_cast<ThreadStartLabel>(lab))
			if (!bLab->getParentCreate().isInitializer())
				recPrintTraceBefore(bLab->getParentCreate(), a, ss);

		/* Do not print the line if it is an RMW write, since it will be
		 * the same as the previous one */
		if (llvm::isa<CasWriteLabel>(lab) || llvm::isa<FaiWriteLabel>(lab))
			continue;
		/* Similarly for a Wna just after the creation of a thread
		 * (it is the store of the PID) */
		if (i > 0 && llvm::isa<ThreadCreateLabel>(g.getPreviousLabel(lab)))
			continue;
		Parser::parseInstFromMData(thr.prefixLOC[i], thr.threadFun->getName().str(), ss);
	}
	return;
}

void GenMCDriver::printTraceBefore(const EventLabel *lab, llvm::raw_ostream &s /* = llvm::dbgs() */)
{
	s << "Trace to " << lab->getPos() << ":\n";

	/* Linearize (po U rf) and print trace */
	View a;
	recPrintTraceBefore(lab->getPos(), a, s);
}

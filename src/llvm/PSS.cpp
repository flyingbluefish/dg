#include <unordered_map>
#include <cassert>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Constant.h>
#include <llvm/Support/CFG.h>
#include <llvm/Support/raw_os_ostream.h>

#include "analysis/PSS.h"
#include "PSS.h"

#ifdef DEBUG_ENABLED
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#endif

namespace dg {
namespace analysis {

#ifdef DEBUG_ENABLED
static std::string
getInstName(const llvm::Value *val)
{
    std::ostringstream ostr;
    llvm::raw_os_ostream ro(ostr);

    assert(val);
    ro << *val;
    ro.flush();

    // break the string if it is too long
    return ostr.str();
}
#endif

struct Subgraph {
    Subgraph(PSSNode *r1, PSSNode *r2, std::pair<PSSNode *, PSSNode *>& a)
        : root(r1), ret(r2), args(a) {}
    Subgraph() {memset(this, 0, sizeof *this);}

    PSSNode *root;
    PSSNode *ret;
    std::pair<PSSNode *, PSSNode *> args;
};

// map of all nodes we created - use to look up operands
std::unordered_map<const llvm::Value *, PSSNode *> nodes_map;
// map of all built subgraphs - the value type is a pair (root, return)
std::unordered_map<const llvm::Value *, Subgraph> subgraphs_map;

enum MemAllocationFuncs {
    NONEMEM = 0,
    MALLOC,
    CALLOC,
    ALLOCA,
};

static int getMemAllocationFunc(const llvm::Function *func)
{
    if (!func || !func->hasName())
        return NONEMEM;

    const char *name = func->getName().data();
    if (strcmp(name, "malloc") == 0)
        return MALLOC;
    else if (strcmp(name, "calloc") == 0)
        return CALLOC;
    else if (strcmp(name, "alloca") == 0)
        return ALLOCA;
    else if (strcmp(name, "realloc") == 0)
        // FIXME
        assert(0 && "realloc not implemented yet");

    return NONEMEM;
}

static inline unsigned getPointerBitwidth(const llvm::DataLayout *DL,
                                          const llvm::Value *ptr)

{
    const llvm::Type *Ty = ptr->getType();
    return DL->getPointerSizeInBits(Ty->getPointerAddressSpace());
}

static uint64_t getAllocatedSize(llvm::Type *Ty, const llvm::DataLayout *DL)
{
    // Type can be i8 *null or similar
    if (!Ty->isSized())
            return 0;

    return DL->getTypeAllocSize(Ty);
}

Pointer getConstantExprPointer(const llvm::ConstantExpr *CE,
                               const llvm::DataLayout *DL);

static Pointer handleConstantBitCast(const llvm::BitCastInst *BC,
                                     const llvm::DataLayout *DL)
{
    using namespace llvm;

    if (!BC->isLosslessCast()) {
        errs() << "WARN: Not a loss less cast unhandled ConstExpr"
               << *BC << "\n";
        abort();
        return PointerUnknown;
    }

    const Value *llvmOp = BC->stripPointerCasts();
    PSSNode *op = nodes_map[llvmOp];
    if (!op) {
        // is this recursively created expression? If so, get the pointer for it
        if (isa<ConstantExpr>(llvmOp)) {
            return getConstantExprPointer(cast<ConstantExpr>(llvmOp), DL);
        } else {
            errs() << *llvmOp << "\n";
            errs() << *BC << "\n";
            assert(0 && "Unsupported bitcast");
        }
    } else {
        assert(op->getType() == pss::CONSTANT
               && "Constant Bitcast on non-constant");

        assert(op->pointsTo.size() == 1
               && "Constant BitCast with not only one pointer");

        return *op->pointsTo.begin();
    }
}

static Pointer handleConstantGep(const llvm::GetElementPtrInst *GEP,
                                 const llvm::DataLayout *DL)
{
    using namespace llvm;

    const Value *op = GEP->getPointerOperand();
    Pointer pointer(UNKNOWN_MEMORY, UNKNOWN_OFFSET);

    // get operand PSSNode - if it exists
    PSSNode *opNode = nodes_map[op];

    // we dont have the operand node... is it constant or constant expr?
    if (!opNode) {
        // is this recursively created expression? If so, get the pointer for it
        if (isa<ConstantExpr>(op)) {
            pointer = getConstantExprPointer(cast<ConstantExpr>(op), DL);
        } else {
            errs() << *op << "\n";
            errs() << *GEP << "\n";
            assert(0 && "Unsupported constant GEP");
        }
    } else {
        assert(opNode->getType() == pss::CONSTANT
                && "ConstantExpr GEP on non-constant");
        assert(opNode->pointsTo.size() == 1
                && "Constant node has more that 1 pointer");
        pointer = *(opNode->pointsTo.begin());
    }

    unsigned bitwidth = getPointerBitwidth(DL, op);
    APInt offset(bitwidth, 0);

    // get offset of this GEP
    if (GEP->accumulateConstantOffset(*DL, offset)) {
        if (offset.isIntN(bitwidth) && !pointer.offset.isUnknown())
            pointer.offset = offset.getZExtValue();
        else
            errs() << "WARN: Offset greater than "
                   << bitwidth << "-bit" << *GEP << "\n";
    }

    return pointer;
}

Pointer getConstantExprPointer(const llvm::ConstantExpr *CE,
                               const llvm::DataLayout *DL)
{
    using namespace llvm;

    Pointer pointer(UNKNOWN_MEMORY, UNKNOWN_OFFSET);
    const Instruction *Inst = const_cast<ConstantExpr*>(CE)->getAsInstruction();

    if (const GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Inst)) {
        pointer = handleConstantGep(GEP, DL);
    } else if (const BitCastInst *BC = dyn_cast<BitCastInst>(Inst)) {
        pointer = handleConstantBitCast(BC, DL);
    } else {
            errs() << "ERR: Unsupported ConstantExpr " << *CE << "\n";
            abort();
    }

    delete Inst;
    return pointer;
}

static PSSNode *createConstantExpr(const llvm::ConstantExpr *CE,
                                   const llvm::DataLayout *DL)
{
    Pointer ptr = getConstantExprPointer(CE, DL);
    PSSNode *node = new PSSNode(pss::CONSTANT, ptr);
    nodes_map[CE] = node;

#ifdef DEBUG_ENABLED
    node->setName(getInstName(CE).c_str());
#endif

    assert(node);
    return node;
}

static PSSNode *getConstant(const llvm::Value *val,
                            const llvm::DataLayout *DL)
{
    if (llvm::isa<llvm::ConstantPointerNull>(val)) {
        return NULLPTR;
    } else if (const llvm::ConstantExpr *CE
                    = llvm::dyn_cast<llvm::ConstantExpr>(val)) {
        return createConstantExpr(CE, DL);
    } else {
        llvm::errs() << "Unspported constant: " << *val << "\n";
        abort();
    }
}

static PSSNode *createDynamicAlloc(const llvm::CallInst *CInst, int type)
{
    using namespace llvm;

    const Value *op;
    uint64_t size = 0, size2 = 0;
    PSSNode *node = new PSSNode(pss::DYN_ALLOC);

    switch (type) {
        case MALLOC:
            node->setIsHeap();
        case ALLOCA:
            op = CInst->getOperand(0);
            break;
        case CALLOC:
            node->setIsHeap();
            node->setZeroInitialized();
            op = CInst->getOperand(1);
            break;
        default:
            errs() << *CInst << "\n";
            assert(0 && "unknown memory allocation type");
    };

    if (const ConstantInt *C = dyn_cast<ConstantInt>(op)) {
        size = C->getLimitedValue();
        // if the size cannot be expressed as an uint64_t,
        // just set it to 0 (that means unknown)
        if (size == ~((uint64_t) 0))
            size = 0;

        // if this is call to calloc, the size is given
        // in the first argument too
        if (type == CALLOC) {
            C = dyn_cast<ConstantInt>(CInst->getOperand(0));
            if (C) {
                size2 = C->getLimitedValue();
                if (size2 == ~((uint64_t) 0))
                    size2 = 0;
                else
                    // OK, if getting the size fails, we end up with
                    // just 1 * size - still better than 0 and UNKNOWN
                    // (it may be cropped later anyway)
                    size *= size2;
            }
        }
    }

    node->setSize(size);
    return node;
}

static std::pair<PSSNode *, PSSNode *>
createDynamicMemAlloc(const llvm::CallInst *CInst, int type)
{
    PSSNode *node = createDynamicAlloc(CInst, type);
    nodes_map[CInst] = node;

#ifdef DEBUG_ENABLED
    node->setName(getInstName(CInst).c_str());
#endif

    // we return (node, node), so that the parent function
    // will seamlessly connect this node into the graph
    return std::make_pair(node, node);
}

static std::pair<PSSNode *, PSSNode *> createOrGetSubgraph(const llvm::CallInst *CInst,
                                                           const llvm::Function *F,
                                                           const llvm::DataLayout *DL)
{
    PSSNode *callNode, *returnNode;

    callNode = new PSSNode(pss::CALL);
    returnNode = new PSSNode(pss::RETURN, callNode);

    nodes_map[CInst] = callNode;
    // NOTE: we do not add return node into nodes_map, since this
    // is artificial node and does not correspond to any real node

#ifdef DEBUG_ENABLED
    callNode->setName(getInstName(CInst).c_str());
    returnNode->setName(("RET" + getInstName(CInst)).c_str());
#endif

    // reuse built subgraphs if available
    Subgraph subg = subgraphs_map[F];
    if (!subg.root) {
        // create new subgraph
        buildLLVMPSS(*F, DL);
        // FIXME: don't find it again, return it from buildLLVMPSS
        // this is redundant
        subg = subgraphs_map[F];
    }

    assert(subg.root && subg.ret);

    // add an edge from last argument to root of the subgraph
    // and from the subprocedure return node (which is one - unified
    // for all return nodes) to return from the call
    callNode->addSuccessor(subg.root);
    subg.ret->addSuccessor(returnNode);

    // add pointers to the arguments PHI nodes
    int idx = 0;
    PSSNode *arg = subg.args.first;
    for (auto A = F->arg_begin(), E = F->arg_end(); A != E; ++A, ++idx) {
        if (A->getType()->isPointerTy()) {
            assert(arg && "BUG: do not have argument");

            PSSNode *op = nodes_map[CInst->getArgOperand(idx)];
            assert(op && "Do not have node for CallInst operand");

            arg->addOperand(op);

            // shift in arguments
            arg = arg->getSingleSuccessor();
        }
    }

    return std::make_pair(callNode, returnNode);
}

// create subgraph or add edges to already existing subgraph,
// return the CALL node (the first) and the RETURN node (the second),
// so that we can connect them into the PSS
static std::pair<PSSNode *, PSSNode *> createCall(const llvm::Instruction *Inst,
                                                  const llvm::DataLayout *DL)
{
    using namespace llvm;
    const CallInst *CInst = cast<CallInst>(Inst);

    const Function *func
        = dyn_cast<Function>(CInst->getCalledValue()->stripPointerCasts());

    if (func) {
        /// memory allocation (malloc, calloc, etc.)
        int type;
        if ((type = getMemAllocationFunc(func))) {
            // NOTE: must be before func->size() == 0 condition,
            // since malloc and similar are undefined too
            return createDynamicMemAlloc(CInst, type);
        } else if (func->size() == 0) {
            // the function is not declared, just put there
            // the call node
            PSSNode *node = new PSSNode(pss::CALL);
#ifdef DEBUG_ENABLED
            node->setName(getInstName(CInst).c_str());
#endif
            return std::make_pair(node, node);
        } else if (func->isIntrinsic()) {
            assert(0 && "Intrinsic function not implemented yet");
        } else {
            return createOrGetSubgraph(CInst, func, DL);
        }
    } else {
        // function pointer call
        assert(0 && "Function pointers not supported yet");
    }
}

static PSSNode *createAlloc(const llvm::Instruction *Inst,
                            const llvm::DataLayout *DL)
{
    PSSNode *node = new PSSNode(pss::ALLOC);
    nodes_map[Inst] = node;

#ifdef DEBUG_ENABLED
    node->setName(getInstName(Inst).c_str());
#endif

    const llvm::AllocaInst *AI = llvm::dyn_cast<llvm::AllocaInst>(Inst);
    if (AI) {
        uint64_t size = getAllocatedSize(AI->getAllocatedType(), DL);
        node->setSize(size);
    }

    assert(node);
    return node;
}

static PSSNode *createStore(const llvm::Instruction *Inst,
                            const llvm::DataLayout *DL)
{
    const llvm::Value *valOp = Inst->getOperand(0);

    PSSNode *op1 = nodes_map[valOp];
    PSSNode *op2 = nodes_map[Inst->getOperand(1)];

    // the value needs to be a pointer - we call this function only under
    // this condition
    assert(valOp->getType()->isPointerTy() && "BUG: Store value is not a pointer");
    assert(op2 && "BUG: Store does not have the pointer operand");

    if (!op1)
        op1 = getConstant(valOp, DL);

    PSSNode *node = new PSSNode(pss::STORE, op1, op2);
    nodes_map[Inst] = node;

#ifdef DEBUG_ENABLED
    node->setName(getInstName(Inst).c_str());
#endif

    assert(node);
    return node;
}

static PSSNode *createLoad(const llvm::Instruction *Inst,
                           const llvm::DataLayout *DL)
{
    const llvm::Value *op = Inst->getOperand(0);
    PSSNode *op1 = nodes_map[op];

    if (!op1)
        op1 = getConstant(op, DL);

    PSSNode *node = new PSSNode(pss::LOAD, op1);
    nodes_map[Inst] = node;

#ifdef DEBUG_ENABLED
    node->setName(getInstName(Inst).c_str());
#endif

    assert(node);
    return node;
}

static PSSNode *createGEP(const llvm::Instruction *Inst,
                          const llvm::DataLayout *DL)
{
    using namespace llvm;

    const GetElementPtrInst *GEP = cast<GetElementPtrInst>(Inst);
    const Value *ptrOp = GEP->getPointerOperand();
    unsigned bitwidth = getPointerBitwidth(DL, ptrOp);
    APInt offset(bitwidth, 0);
    PSSNode *node = nullptr;

    PSSNode *op = nodes_map[ptrOp];
    if (!op) {
        if (const llvm::ConstantExpr *CE
                = llvm::dyn_cast<llvm::ConstantExpr>(ptrOp)) {
            op = createConstantExpr(CE, DL);
        } else {
            llvm::errs() << *ptrOp << "\n";
            llvm::errs() << *Inst << "\n";
            assert(0 && "Instruction unspported");
        }
    }

    if (GEP->accumulateConstantOffset(*DL, offset)) {
        if (offset.isIntN(bitwidth))
            node = new PSSNode(pss::GEP, op, offset.getZExtValue());
        else
            errs() << "WARN: GEP offset greater than " << bitwidth << "-bit";
            // fall-through to UNKNOWN_OFFSET in this case
    }

    if (!node)
        node = new PSSNode(pss::GEP, op, UNKNOWN_OFFSET);

    nodes_map[Inst] = node;

#ifdef DEBUG_ENABLED
    node->setName(getInstName(Inst).c_str());
#endif

    assert(node);
    return node;
}

static PSSNode *createCast(const llvm::Instruction *Inst,
                           const llvm::DataLayout *DL)
{
    const llvm::Value *op = Inst->getOperand(0);
    PSSNode *op1 = nodes_map[op];

    if (!op1) {
        if (const llvm::ConstantExpr *CE
                = llvm::dyn_cast<llvm::ConstantExpr>(op)) {
            op1 = createConstantExpr(CE, DL);
        } else {
            llvm::errs() << *op << "\n";
            llvm::errs() << *Inst << "\n";
            assert(0 && "Instruction unspported");
        }
    }

    PSSNode *node = new PSSNode(pss::CAST, op1);
    nodes_map[Inst] = node;

#ifdef DEBUG_ENABLED
    node->setName(getInstName(Inst).c_str());
#endif

    assert(node);
    return node;
}

// return first and last nodes of the block
std::pair<PSSNode *, PSSNode *> buildPSSBlock(const llvm::BasicBlock& block,
                                              const llvm::DataLayout *DL)
{
    using namespace llvm;

    std::pair<PSSNode *, PSSNode *> ret(nullptr, nullptr);
    PSSNode *prev_node;
    PSSNode *node = nullptr;
    for (const Instruction& Inst : block) {
        prev_node = node;

        switch(Inst.getOpcode()) {
            case Instruction::Alloca:
                node = createAlloc(&Inst, DL);
                break;
            case Instruction::Store:
                // create only nodes that store pointer to another
                // pointer. We don't care about stores of non-pointers
                if (Inst.getOperand(0)->getType()->isPointerTy())
                    node = createStore(&Inst, DL);
                break;
            case Instruction::Load:
                if (Inst.getType()->isPointerTy())
                    node = createLoad(&Inst, DL);
                break;
            case Instruction::GetElementPtr:
                node = createGEP(&Inst, DL);
                break;
            case Instruction::BitCast:
                node = createCast(&Inst, DL);
                break;
            case Instruction::Call:
                std::pair<PSSNode *, PSSNode *> subg = createCall(&Inst, DL);
                if (prev_node)
                    prev_node->addSuccessor(subg.first);
                else
                    // graphs starts with function call?
                    ret.first = subg.first;

                // new nodes will connect to the return node
                node = prev_node = subg.second;

                break;
        }

        // first instruction
        if (node && !prev_node)
            ret.first = node;

        if (prev_node && prev_node != node)
            prev_node->addSuccessor(node);
    }

    // last node
    ret.second = node;

    return ret;
}

static size_t blockAddSuccessors(std::map<const llvm::BasicBlock *,
                                          std::pair<PSSNode *, PSSNode *>>& built_blocks,
                                 std::pair<PSSNode *, PSSNode *>& pssn,
                                 const llvm::BasicBlock& block)
{
    size_t num = 0;

    for (llvm::succ_const_iterator
         S = llvm::succ_begin(&block), SE = llvm::succ_end(&block); S != SE; ++S) {
        std::pair<PSSNode *, PSSNode *>& succ = built_blocks[*S];
        assert((succ.first && succ.second) || (!succ.first && !succ.second));
        if (!succ.first) {
            // if we don't have this block built (there was no points-to
            // relevant instruction), we must pretend to be there for
            // control flow information. Thus instead of adding it as
            // successor, add its successors as successors
            num += blockAddSuccessors(built_blocks, pssn, *(*S));
        } else {
            // add successor to the last nodes
            pssn.second->addSuccessor(succ.first);
            ++num;
        }
    }

    return num;
}

static std::pair<PSSNode *, PSSNode *> buildArguments(const llvm::Function& F)
{
    // create PHI nodes for arguments of the function. These will be
    // successors of call-node
    std::pair<PSSNode *, PSSNode *> ret;
    int idx = 0;
    PSSNode *prev, *arg = nullptr;

    for (auto A = F.arg_begin(), E = F.arg_end(); A != E; ++A, ++idx) {
        if (A->getType()->isPointerTy()) {
            prev = arg;

            arg = new PSSNode(pss::PHI, nullptr);
            nodes_map[&*A] = arg;

            if (prev)
                prev->addSuccessor(arg);
            else
                ret.first = arg;

#ifdef DEBUG_ENABLED
            arg->setName(("ARG phi " + getInstName(&*A)).c_str());
#endif
        }
    }

    ret.second = arg;
    assert((ret.first && ret.second) || (!ret.first && !ret.second));

    return ret;
}


// build pointer state subgraph for given graph
// \return   root node of the graph
PSSNode *buildLLVMPSS(const llvm::Function& F, const llvm::DataLayout *DL)
{
    // here we'll keep first and last nodes of every built block and
    // connected together according to successors
    std::map<const llvm::BasicBlock *, std::pair<PSSNode *, PSSNode *>> built_blocks;
    PSSNode *lastNode = nullptr;

    // create root and (unified) return nodes of this subgraph. These are
    // just for our convenience when building the graph, they can be
    // optimized away later since they are noops
    PSSNode *root = new PSSNode(pss::NOOP);
    PSSNode *ret = new PSSNode(pss::NOOP);

#ifdef DEBUG_ENABLED
    root->setName((std::string("entry ") + F.getName().data()).c_str());
    ret->setName((std::string("ret ") + F.getName().data()).c_str());
#endif

    // now build the arguments of the function - if it has any
    std::pair<PSSNode *, PSSNode *> args = buildArguments(F);

    // add record to built graphs here, so that subsequent call of this function
    // from buildPSSBlock won't get stuck in infinite recursive call when
    // this function is recursive
    subgraphs_map[&F] = Subgraph(root, ret, args);

    // make arguments the entry block of the subgraphs (if there
    // are any arguments)
    if (args.first) {
        root->addSuccessor(args.first);
        lastNode = args.second;
    } else
        lastNode = root;

    assert(lastNode);

    PSSNode *first = nullptr;
    for (const llvm::BasicBlock& block : F) {
        std::pair<PSSNode *, PSSNode *> nds = buildPSSBlock(block, DL);

        if (!first) {
            // first block was not created at all? (it has not
            // pointer relevant instructions) - in that case
            // fake that the first block is the root itself
            if (!nds.first) {
                // if the function has arguments, then it has
                // single entry block where it copies the values
                // of arguments to local variables - thus this
                // assertions must hold
                assert(!args.first);
                assert(lastNode == root);

                nds.first = nds.second = root;
                first = root;
            } else {
                first = nds.first;

                // add correct successors. If we have arguments,
                // then connect the first block after arguments.
                // Otherwise connect them after the root node
                lastNode->addSuccessor(first);
            }
        }

        built_blocks[&block] = nds;
    }

    std::vector<PSSNode *> rets;
    for (const llvm::BasicBlock& block : F) {
        std::pair<PSSNode *, PSSNode *>& pssn = built_blocks[&block];
        // if the block do not contain any points-to relevant instruction,
        // we returned (nullptr, nullptr)
        // FIXME: do not store such blocks at all
        assert((pssn.first && pssn.second) || (!pssn.first && !pssn.second));
        if (!pssn.first)
            continue;

        // add successors to this block (skipping the empty blocks)
        size_t succ_num = blockAddSuccessors(built_blocks, pssn, block);

        // if we have not added any successor, then the last node
        // of this block is a return node
        if (succ_num == 0)
            rets.push_back(pssn.second);
    }

    // add successors edges from every real return to our artificial ret node
    assert(!rets.empty() && "BUG: Did not find any return node in function");
    for (PSSNode *r : rets)
        r->addSuccessor(ret);

    return root;
}

static void handleGlobalVariableInitializer(const llvm::Constant *C,
                                            PSSNode *node)
{
    using namespace llvm;

    if (isa<ConstantPointerNull>(C)) {
        node->setZeroInitialized();
    } else if (!isa<ConstantInt>(C)) {
        llvm::errs() << *C << "\n";
        llvm::errs() << "ERROR: ^^^ global variable initializer not handled\n";
    }
}

std::pair<PSSNode *, PSSNode *> buildGlobals(const llvm::Module *M,
                                             const llvm::DataLayout *DL)
{
    PSSNode *cur = nullptr, *prev, *first = nullptr;
    for (auto I = M->global_begin(), E = M->global_end(); I != E; ++I) {
        prev = cur;

        // every global node is like memory allocation
        cur = new PSSNode(pss::ALLOC);
        nodes_map[&*I] = cur;

#ifdef DEBUG_ENABLED
        cur->setName(getInstName(&*I).c_str());
#endif

        // handle globals initialization
        const llvm::GlobalVariable *GV
                            = llvm::dyn_cast<llvm::GlobalVariable>(&*I);
        if (GV && GV->hasInitializer() && !GV->isExternallyInitialized()) {
            const llvm::Constant *C = GV->getInitializer();
            handleGlobalVariableInitializer(C, cur);
        }

        if (prev)
            prev->addSuccessor(cur);
        else
            first = cur;
    }

    assert((!first && !cur) || (first && cur));
    return std::pair<PSSNode *, PSSNode *>(first, cur);
}

} // namespace analysis
} // namespace dg
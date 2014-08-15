//===- EraseNonkernel.cpp - Remove non-GPU codes from LLVM IR -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass which removes non-GPU codes from LLVM IR.
//
//===----------------------------------------------------------------------===//

//#define DEBUG_TYPE "PromoteGlobals"

#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/CallingConv.h"
using namespace llvm;


namespace {

/* Data type container into which the list of LLVM functions
   that are OpenCL kernels will be stored */
typedef SmallVector<Function *, 3> FunctionVect;

/* The name of the MDNode into which the list of
   MD nodes referencing each OpenCL kernel is stored. */
static Twine KernelListMDNodeName = "opencl.kernels";

/* Find the MDNode which reference the list of opencl kernels.
   NULL if it does not exists. */

NamedMDNode * getKernelListMDNode (Module & M)
{
        return M.getNamedMetadata(KernelListMDNodeName);
}

/* A visitor function which is called for each MDNode
   located into another MDNode */
class KernelNodeVisitor {
        public:
        KernelNodeVisitor(FunctionVect& FV);
        void operator()(MDNode * N);

        private:
        FunctionVect& found_kernels;
};

KernelNodeVisitor::KernelNodeVisitor(FunctionVect& FV)
        : found_kernels(FV)
{}

void KernelNodeVisitor::operator()(MDNode *N)
{
        if ( N->getNumOperands() < 1) return;
        Value * Op = N->getOperand(0);
        if ( Function * F = dyn_cast<Function>(Op)) {
                found_kernels.push_back(F); 
        }
}

/* Call functor for each MDNode located within the Named MDNode */
void visitMDNodeOperands(NamedMDNode * N, KernelNodeVisitor& visitor)
{
        for (unsigned operand = 0, end = N->getNumOperands();
             operand < end; ++operand) {
                visitor(N->getOperand(operand));
        }
}

/* Accumulate LLVM functions that are kernels within the
   found_kernels vector. Return true if kernels are found.
   False otherwise. */
bool findKernels(Module& M, FunctionVect& found_kernels)
{
        NamedMDNode * root = getKernelListMDNode(M);
        if (!root || (root->getNumOperands() == 0)) return false;

        KernelNodeVisitor visitor(found_kernels);
        visitMDNodeOperands(root, visitor);

        return found_kernels.size() != 0;
}

class EraseNonkernels : public ModulePass {
        FunctionVect foundKernels;
        public:
        static char ID;
        EraseNonkernels();
        virtual ~EraseNonkernels();
        void getAnalysisUsage(AnalysisUsage& AU) const;
        bool runOnModule(Module& M);
};
} // ::<unnamed> namespace

EraseNonkernels::EraseNonkernels() : ModulePass(ID)
{}

EraseNonkernels::~EraseNonkernels()
{}

void EraseNonkernels::getAnalysisUsage(AnalysisUsage& AU) const
{
        AU.addRequired<CallGraph>();
}

bool EraseNonkernels::runOnModule(Module &M)
{
    findKernels(M, foundKernels);

    typedef Module::iterator func_iterator;
    for (func_iterator F = M.begin(), Fend = M.end();
            F != Fend; ++F) {
        typedef FunctionVect::const_iterator kernel_iterator;
        bool isKernel = false;
        if (!foundKernels.empty()) {
            for (kernel_iterator KernFunc = foundKernels.begin(), KernFuncEnd = foundKernels.end();
                    KernFunc != KernFuncEnd; ++KernFunc) {
                if (*KernFunc == &*F) {
                    isKernel = true;
                }
            }
        }

        if (isKernel) continue;

        F->deleteBody();
    }

    foundKernels.clear();
    // Some global variables may be dead after removing dead kernels
    Module::GlobalListType &globals = M.getGlobalList();
    for (Module::global_iterator I = globals.begin(), E = globals.end();
            I != E; ) {
        I->removeDeadConstantUsers();
        if (I->getNumUses() == 0) {
            I->eraseFromParent();
            I = globals.begin();
        } else {
            I++;
        }
    }

	// codes below will remove CPU codes emitted in GPU path
	// only functions in the following 3 categories would be preserved:
	// 1) kernels : functions with SPIR_KERNEL linkage
	// 2) OpenCL intrinsics
	// 3) LLVM intrinsics

	// remove unwanted functions
	Module::FunctionListType &global_funcs = M.getFunctionList();
	for (Module::iterator I = global_funcs.begin(), E = global_funcs.end(); I != E; ) {
		// keep functions with SPIR_KERNEL linkage
		if (I->getCallingConv() == CallingConv::SPIR_KERNEL) {
			I++;
			continue;
		}

		// keep certain intrinsics
		// FIXME: switch to attribute-based check
		if (I->getName().find("get_global_id") != StringRef::npos ||
			I->getName().find("get_local_id") != StringRef::npos ||
			I->getName().find("get_group_id") != StringRef::npos ||
			I->getName().find("barrier") != StringRef::npos ||
			I->getName().find("opencl_") != StringRef::npos ||
			I->getName().find("atomic_") != StringRef::npos ||
			I->getName().find("llvm.") != StringRef::npos || 
                        I->getName().find("_Znwj") != StringRef::npos || 
                        I->getName().find("_Znaj") != StringRef::npos ||
                        I->getName().find("_ZdlPv") != StringRef::npos ||
                        I->getName().find("_ZdaPv") != StringRef::npos ) {
			I++;
			continue;
		}

		// remove all other functions
		//llvm::errs() << "[REMOVE FUNC]: " << I->getName() << "\n";
		I->removeFromParent();
		I = global_funcs.begin();
	}

	// remove unwanted aliases
	Module::AliasListType &global_aliases = M.getAliasList();
	for (Module::alias_iterator I = global_aliases.begin(), E = global_aliases.end(); I != E; ) {
		I->removeDeadConstantUsers();
		if (I->getNumUses() == 0) {
			//llvm::errs() << "[REMOVE ALIAS]: " << I->getName() << "\n";
			I->eraseFromParent();
			I = global_aliases.begin();
		} else {
			I++;
		}
	}

    return true;
}

char EraseNonkernels::ID = 0;
static RegisterPass<EraseNonkernels>
Y("erase-nonkernels", "Erase body of all functions not marked kernel in metadata.");

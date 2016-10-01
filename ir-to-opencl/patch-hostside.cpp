// Copyright Hugh Perkins 2016

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


// This is going to patch the cuda launch instrutions, in the hostside ir. hopefully

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <fstream>

#include "ir-to-opencl-common.h"

using namespace llvm;
using namespace std;

static llvm::LLVMContext TheContext;
static llvm::IRBuilder<> Builder(TheContext);
static std::unique_ptr<llvm::Module> TheModule;

bool single_precision = true;

class LaunchCallInfo {
public:
    LaunchCallInfo() {
        // launchInstruction = 0;
        // kernelName = "";
        for(int i = 0; i < 3; i++) {
            grid[i] = 0;
            block[i] = 0;
        }
    }
    std::string kernelName = "";
    // CallInst *launchInstruction;
    vector<Type *> callTypes;
    vector<Value *> callValues;
    int grid[3];
    int block[3];
};

ostream &operator<<(ostream &os, const LaunchCallInfo &info) {
    raw_os_ostream my_raw_os_ostream(os);
    my_raw_os_ostream << "LaunchCallInfo " << info.kernelName;
    my_raw_os_ostream << "<<<";

    my_raw_os_ostream << "dim3(";
    for(int j = 0; j < 3; j++) {
        if(j > 0) {
            my_raw_os_ostream << ", ";
        }
        my_raw_os_ostream << info.grid[j];
    }
    my_raw_os_ostream << ")";
    my_raw_os_ostream << ", ";

    my_raw_os_ostream << "dim3(";
    for(int j = 0; j < 3; j++) {
        if(j > 0) {
            my_raw_os_ostream << ", ";
        }
        my_raw_os_ostream << info.block[j];
    }
    my_raw_os_ostream << ")";

    my_raw_os_ostream << ">>>";
    my_raw_os_ostream << "(";
    int i = 0;
    for(auto it=info.callTypes.begin(); it != info.callTypes.end(); it++) {
        if(i > 0){
            my_raw_os_ostream << ", ";
        }
        Type *type = *it;
        type->print(my_raw_os_ostream);
        i++;
    }
    my_raw_os_ostream << ");\n";
    for(auto it=info.callValues.begin(); it != info.callValues.end(); it++) {
        Value *value = *it;
        my_raw_os_ostream << "value " << dumpType(value->getType()) << "\n";
    }
    return os;
}

void getLaunchTypes(CallInst *inst, LaunchCallInfo *info) {
    // unique_ptr<LaunchCallInfo> launchCallInfo(new LaunchCallInfo);
    Value *argOperand = inst->getArgOperand(0);
    if(ConstantExpr *expr = dyn_cast<ConstantExpr>(argOperand)) {
        Instruction *instr = expr->getAsInstruction();
        Type *op0type = instr->getOperand(0)->getType();
        Type *op0typepointed = op0type->getPointerElementType();
        if(FunctionType *fn = dyn_cast<FunctionType>(op0typepointed)) {
            for(auto it=fn->param_begin(); it != fn->param_end(); it++) {
                Type * paramType = *it;
                info->callTypes.push_back(paramType);
            }
        }
        info->kernelName = instr->getOperand(0)->getName();
    }
    // return launchCallInfo;
}

void getLaunchArgValue(CallInst *inst, LaunchCallInfo *info) {
    Instruction *op0 = dyn_cast<Instruction>(inst->getOperand(0));
    Instruction *op0_0 = dyn_cast<Instruction>(op0->getOperand(0));
    for(auto it=op0_0->user_begin(); it != op0_0->user_end(); it++) {
        if(Instruction *useInst = dyn_cast<Instruction>(*it)) {
            if(StoreInst *store = dyn_cast<StoreInst>(useInst)) {
                // cout << "store operand 0 type " << dumpType(store->getOperand(0)->getType()) << endl;
                info->callValues.push_back(store->getOperand(0));
            }
        }
    }
}

uint64_t readIntConstant_uint64(ConstantInt *constant) {
    return constant->getZExtValue();
}

uint32_t readIntConstant_uint32(ConstantInt *constant) {
    assert(contant->getBitWidth() <= 32);
    return (uint32_t)constant->getZExtValue();
}

void getBlockGridDimensions(CallInst *inst, LaunchCallInfo *info) {
    // there are 6 args:
    // grid:
    // 0 i64: x, y, as 32-bit ints
    // 1 i32: z
    // block:
    // 2 i64: x, y, as 32-bit ints
    // 3 i32: z
    // 4 shared memory.  since we're not handling it right now, must be 0
    // 5 stream must be null, for now

    uint64_t grid_xy = readIntConstant_uint64(cast<ConstantInt>(inst->getArgOperand(0)));
    uint32_t grid_x = grid_xy & ((1 << 31) - 1);
    uint32_t grid_y = grid_xy >> 32;
    uint32_t grid_z = readIntConstant_uint32(cast<ConstantInt>(inst->getArgOperand(1)));
    // cout << "grid " << grid_x << " " << grid_y << " " << grid_z << endl;

    uint64_t block_xy = readIntConstant_uint64(cast<ConstantInt>(inst->getArgOperand(2)));
    uint32_t block_x = block_xy & ((1 << 31) - 1);
    uint32_t block_y = block_xy >> 32;
    uint32_t block_z = readIntConstant_uint32(cast<ConstantInt>(inst->getArgOperand(3)));
    // cout << "block " << block_x << " " << block_y << " " << block_z << endl;

    info->grid[0] = grid_x;
    info->grid[1] = grid_y;
    info->grid[2] = grid_z;

    info->block[0] = block_x;
    info->block[1] = block_y;
    info->block[2] = block_z;

    assert(readIntConstant_uint64(inst->getArgOperand(4)) == 0);
    // we should assert on the stream too really TODO: FIXME:
    // assert(readIntConstant_uint64(inst->getArgOperand(5)) == 0);
}

void patchFunction(Function *F) {
    vector<Instruction *> to_erase;
    unique_ptr<LaunchCallInfo> launchCallInfo(new LaunchCallInfo);
    vector<Instruction *> to_replace_with_zero;
    IntegerType *inttype = IntegerType::get(TheContext, 32);
    ConstantInt *constzero = ConstantInt::getSigned(inttype, 0);
    for(auto it=F->begin(); it != F->end(); it++) {
        BasicBlock *basicBlock = &*it;
        for(auto insit=basicBlock->begin(); insit != basicBlock->end(); insit++) {
            if(CallInst *inst = dyn_cast<CallInst>(&*insit)) {
                Function *called = inst->getCalledFunction();
                if(called == 0) {
                    continue;
                }
                if(!called->hasName()) {
                    continue;
                }
                string calledFunctionName = called->getName();
                if(calledFunctionName == "cudaLaunch") {
                    getLaunchTypes(inst, launchCallInfo.get());
                    to_erase.push_back(inst);
                    cout << *launchCallInfo << endl;
                    launchCallInfo.reset(new LaunchCallInfo);
                } else if(calledFunctionName == "cudaSetupArgument") {
                    getLaunchArgValue(inst, launchCallInfo.get());
                    to_replace_with_zero.push_back(inst);
                } else if(calledFunctionName == "cudaConfigureCall") {
                    // cout << "got call to cudaconfigurecall" << endl;
                    getBlockGridDimensions(inst, launchCallInfo.get());
                    to_replace_with_zero.push_back(inst);
                }
            }
        }
    }
    cout << *launchCallInfo << endl;
    for(auto it=to_erase.begin(); it != to_erase.end(); it++) {
        Instruction *inst = *it;
        if(!inst->use_empty()) {
            throw runtime_error("cannot erase used instructions");
        }
        inst->eraseFromParent();
    }
    for(auto it=to_replace_with_zero.begin(); it != to_replace_with_zero.end(); it++) {
        Instruction *inst = *it;
        BasicBlock::iterator ii(inst);
        ReplaceInstWithValue(inst->getParent()->getInstList(), ii, constzero);
        cout << "after replacevalue" << endl;
    }

    // if(launchCallInfo != 0 && launchCallInfo->launchInstruction != 0) {
    //     cout << "erasing" << endl;
    //     launchCallInfo->launchInstruction->eraseFromParent();
    // }
}


void patchModule(Module *M) {
    int i = 0;
    for(auto it = M->begin(); it != M->end(); it++) {
        // nameByValue.clear();
        // nextNameIdx = 0;
        string name = it->getName();
        // cout << "name " << name << endl;
        Function *F = &*it;
        if(name == "_Z14launchSetValuePfif") {
            cout << "Function " << name << endl;
            patchFunction(F);
            cout << "verifying function..." << endl;
            verifyFunction(*F);
            cout << "function verified" << endl;
        }
        // if(ignoredFunctionNames.find(name) == ignoredFunctionNames.end() &&
        //         knownFunctionsMap.find(name) == knownFunctionsMap.end()) {
        //     Function *F = &*it;
        //     if(i > 0) {
        //         cout << endl;
        //     }
        //     dumpFunction(F);
        //     i++;
        // }
    }
}

int main(int argc, char *argv[]) {
    SMDiagnostic Err;
    if(argc != 3) {
        cout << "Usage: " << argv[0] << " infile.ll outfile.o" << endl;
        return 1;
    }
    string infile = argv[1];
    // debug = false;
    // if(argc == 3) {
    //     if(string(argv[1]) != "--debug") {
    //         cout << "Usage: " << argv[0] << " [--debug] target.ll" << endl;
    //         return 1;
    //     } else {
    //         debug = true;
    //     }
    // }
    TheModule = parseIRFile(infile, Err, TheContext);
    if(!TheModule) {
        Err.print(argv[0], errs());
        return 1;
    }

    patchModule(TheModule.get());

    AssemblyAnnotationWriter assemblyAnnotationWriter;
    ofstream ofile;
    ofile.open(argv[2]);
    raw_os_ostream my_raw_os_ostream(ofile);
    verifyModule(*TheModule);
    cout << "printing module" << endl;
    TheModule->print(my_raw_os_ostream, &assemblyAnnotationWriter);
    // my_raw_os_ostream.close();
    ofile.close();
    // TheModule->dump();
//    dumpModule(TheModule.get());
    return 0;
}

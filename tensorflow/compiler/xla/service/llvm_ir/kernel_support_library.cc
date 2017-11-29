/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/llvm_ir/kernel_support_library.h"

#include "tensorflow/compiler/xla/service/llvm_ir/llvm_loop.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_util.h"

namespace xla {
void KernelSupportLibrary::For(
    tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
    llvm::Value* step,
    const std::function<void(llvm::Value*, bool)>& for_body_generator) {
  If(ir_builder_->CreateICmpSLT(start, end), [&]() {
    for_body_generator(start, /*is_first_iteration=*/true);
    For(name, ir_builder_->CreateAdd(start, step), end, step,
        [&](llvm::Value* iv) { for_body_generator(iv, false); });
  });
}

void KernelSupportLibrary::For(
    tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
    llvm::Value* step, bool peel_first_iteration,
    const std::function<void(llvm::Value*, llvm::Value*)>& for_body_generator) {
  if (peel_first_iteration) {
    For(name, start, end, step, true,
        [&](llvm::Value* indvar, bool is_first_iteration) {
          for_body_generator(indvar, ir_builder_->getInt1(is_first_iteration));
        });
  } else {
    std::unique_ptr<llvm_ir::ForLoop> loop = llvm_ir::ForLoop::EmitForLoop(
        name, start, end, step, ir_builder_,
        /*prevent_unrolling=*/prevent_unrolling_,
        /*prevent_vectorization=*/prevent_vectorization_);
    ir_builder_->SetInsertPoint(&loop->GetBodyBasicBlock()->back());
    for_body_generator(loop->GetIndVarValue(),
                       /*is_first_iteration=*/ir_builder_->CreateICmpEQ(
                           loop->GetIndVarValue(), start));
    llvm_ir::SetToLastInsertPoint(loop->GetExitBasicBlock(), ir_builder_);
  }
}

void KernelSupportLibrary::If(
    llvm::Value* condition, const std::function<void()>& true_block_generator,
    const std::function<void()>& false_block_generator) {
  llvm_ir::LlvmIfData if_data =
      llvm_ir::EmitIfThenElse(condition, "", ir_builder_);
  ir_builder_->SetInsertPoint(&if_data.true_block->back());
  true_block_generator();
  ir_builder_->SetInsertPoint(&if_data.false_block->back());
  false_block_generator();
  llvm_ir::SetToLastInsertPoint(if_data.after_block, ir_builder_);
}

void KernelSupportLibrary::EmitAndCallOutlinedKernel(
    llvm::IRBuilder<>* ir_builder, tensorflow::StringPiece kernel_name,
    KernelSupportLibrary::ArgumentVector arguments,
    const std::function<void(KernelSupportLibrary::ArgumentVector)>&
        kernel_body_generator) {
  llvm::Module* module = ir_builder->GetInsertBlock()->getModule();
  llvm::Function* function =
      module->getFunction(llvm_ir::AsStringRef(kernel_name));
  if (!function) {
    VLOG(2) << "Generating kernel for " << kernel_name;
    std::vector<llvm::Type*> arg_types;
    std::transform(arguments.begin(), arguments.end(),
                   std::back_inserter(arg_types),
                   [](llvm::Value* arg) { return arg->getType(); });

    auto* function_type = llvm::FunctionType::get(
        ir_builder->getVoidTy(), arg_types, /*isVarArg=*/false);

    function = llvm::Function::Create(
        function_type, llvm::GlobalValue::InternalLinkage,
        llvm_ir::AsStringRef(kernel_name), module);

    llvm::IRBuilder<>::InsertPointGuard guard(*ir_builder);

    auto* entry_bb =
        llvm::BasicBlock::Create(ir_builder->getContext(), "entry", function);
    auto* return_inst = llvm::ReturnInst::Create(ir_builder->getContext(),
                                                 /*retVal=*/nullptr, entry_bb);
    // Set the insert point to before return_inst.
    ir_builder->SetInsertPoint(return_inst);

    std::vector<llvm::Value*> arg_values;
    std::transform(function->arg_begin(), function->arg_end(),
                   std::back_inserter(arg_values), std::addressof<llvm::Value>);
    kernel_body_generator(arg_values);
  } else {
    VLOG(3) << "Re-using kernel for " << kernel_name;
  }

  ir_builder->CreateCall(function, llvm_ir::AsArrayRef(arguments));
}

}  // namespace xla

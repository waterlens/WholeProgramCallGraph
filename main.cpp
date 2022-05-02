#include <fmt/core.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <system_error>

#include "CallGraph.h"

using namespace llvm;

int main(int argc, char *argv[]) {
  StringRef file = argc > 1 ? argv[1] : "program.bc";
  LLVMContext context;

  auto fileOrErr = MemoryBuffer::getFileOrSTDIN(file);
  if (std::error_code ec = fileOrErr.getError()) {
    fmt::print("Error opening input file: {}\n", ec.message());
    return 1;
  }

  auto moduleOrErr =
      parseBitcodeFile(fileOrErr->get()->getMemBufferRef(), context);
  if (Error err = moduleOrErr.takeError()) {
    if (!err.success()) {
      fmt::print("Error reading bitcode\n");
      return 1;
    }
  }

  auto *module = moduleOrErr->get();
  CallGraph callgraph(*module);
  callgraph.print();
}
// Sandstorm Blackrock
// Copyright (c) 2015 Sandstorm Development Group, Inc.
// All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "empty-driver.h"
#include <kj/debug.h>
#include <unistd.h>
#include <fcntl.h>
#include <sandstorm/util.h>
#include <capnp/serialize-async.h>

namespace blackrock {

namespace {

    // TODO(cleanup): Share this code with version in master.c++.
    kj::Promise<kj::String> readAllAsync(kj::AsyncInputStream& input,
                                        kj::Vector<char> buffer = kj::Vector<char>()) {
    buffer.resize(buffer.size() + 4096);
    auto promise = input.tryRead(buffer.end() - 4096, 4096, 4096);
    return promise.then([KJ_MVCAP(buffer),&input](size_t n) mutable -> kj::Promise<kj::String> {
        if (n < 4096) {
        buffer.resize(buffer.size() - 4096 + n);
        buffer.add('\0');
        return kj::String(buffer.releaseAsArray());
        } else {
        return readAllAsync(input, kj::mv(buffer));
        }
    });
    }

}  // namespace

EmptyDriver::EmptyDriver(sandstorm::SubprocessSet& subprocessSet,
                     kj::LowLevelAsyncIoProvider& ioProvider)
    : subprocessSet(subprocessSet), ioProvider(ioProvider),
      masterBindAddress(SimpleAddress::getInterfaceAddress(AF_INET, "eth0")),
      logTask(nullptr), logSinkAddress(masterBindAddress) {
  // Create socket for the log sink acceptor.
  int sock;
  KJ_SYSCALL(sock = socket(masterBindAddress.family(),
      SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  {
    KJ_ON_SCOPE_FAILURE(close(sock));
    logSinkAddress.setPort(0);
    KJ_SYSCALL(bind(sock, logSinkAddress.asSockaddr(), logSinkAddress.getSockaddrSize()));
    KJ_SYSCALL(listen(sock, SOMAXCONN));

    // Read back the assigned port number.
    logSinkAddress = SimpleAddress::getLocal(sock);
  }

  // Accept log connections.
  auto listener = ioProvider.wrapListenSocketFd(sock,
      kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP |
      kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC |
      kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK);

  logTask = logSink.acceptLoop(kj::mv(listener))
      .eagerlyEvaluate([](kj::Exception&& exception) {
    KJ_LOG(ERROR, "LogSink accept loop failed", exception);
  });
}

EmptyDriver::~EmptyDriver() noexcept(false) {}

SimpleAddress EmptyDriver::getMasterBindAddress() {
  return masterBindAddress;
}

auto EmptyDriver::listMachines() -> kj::Promise<kj::Array<MachineId>> {
  int fds[2];
  KJ_SYSCALL(pipe2(fds, O_CLOEXEC));
  kj::AutoCloseFd writeEnd(fds[1]);
  auto input = ioProvider.wrapInputFd(fds[0],
      kj::LowLevelAsyncIoProvider::Flags::TAKE_OWNERSHIP |
      kj::LowLevelAsyncIoProvider::Flags::ALREADY_CLOEXEC);

  auto exitPromise = gceCommand({"echo","list-all-machine"}, STDIN_FILENO, writeEnd);

  auto outputPromise = readAllAsync(*input);
  return outputPromise.attach(kj::mv(input))
      .then([this,KJ_MVCAP(exitPromise)](kj::String allText) mutable {

    kj::Vector<MachineId> result;
    kj::Vector<kj::Promise<void>> promises;

    promises.add(kj::mv(exitPromise));

    return kj::joinPromises(promises.releaseAsArray())
        .then([KJ_MVCAP(result)]() mutable { return result.releaseAsArray(); });
  });
}

kj::Promise<void> EmptyDriver::boot(MachineId id) {
  kj::String name = kj::str(id);
  kj::Vector<kj::StringPtr> args;
  args.addAll(std::initializer_list<const kj::StringPtr> { "echo", name, "NEED-BOOT" });
  return gceCommand(args);
}

kj::Promise<VatPath::Reader> EmptyDriver::run(
    MachineId id, blackrock::VatId::Reader masterVatId, bool requireRestartProcess) {
  kj::String name = kj::str(id);

  int fds[2];
  KJ_SYSCALL(pipe2(fds, O_CLOEXEC));
  kj::AutoCloseFd stdinReadEnd(fds[0]);
  auto stdinWriteEnd = ioProvider.wrapOutputFd(fds[1],
      kj::LowLevelAsyncIoProvider::Flags::TAKE_OWNERSHIP |
      kj::LowLevelAsyncIoProvider::Flags::ALREADY_CLOEXEC);
  KJ_SYSCALL(pipe2(fds, O_CLOEXEC));
  kj::AutoCloseFd stdoutWriteEnd(fds[1]);
  auto stdoutReadEnd = ioProvider.wrapInputFd(fds[0],
      kj::LowLevelAsyncIoProvider::Flags::TAKE_OWNERSHIP |
      kj::LowLevelAsyncIoProvider::Flags::ALREADY_CLOEXEC);

  auto addr = kj::str(logSinkAddress, '/', name);
  auto target = kj::str("root@", name);
  kj::Vector<kj::StringPtr> args;
  auto command = kj::str("/blackrock/bin/blackrock slave --log ", addr, " if4:eth0");
  args.addAll(kj::ArrayPtr<const kj::StringPtr>({
      "ssh", target, command}));

  auto exitPromise = gceCommand(args, stdinReadEnd, stdoutWriteEnd);

  auto message = kj::heap<capnp::MallocMessageBuilder>(masterVatId.totalSize().wordCount + 4);
  message->setRoot(masterVatId);

  auto& stdoutReadEndRef = *stdoutReadEnd;
  return capnp::writeMessage(*stdinWriteEnd, *message)
      .attach(kj::mv(stdinWriteEnd), kj::mv(message))
      .then([&stdoutReadEndRef]() {
    return capnp::readMessage(stdoutReadEndRef);
  }).then([this,id,KJ_MVCAP(exitPromise),KJ_MVCAP(stdoutReadEnd)](
      kj::Own<capnp::MessageReader> reader) mutable {
    auto path = reader->getRoot<VatPath>();
    vatPaths[id] = kj::mv(reader);
    return exitPromise.then([path]() { return path; });
  });
}

kj::Promise<void> EmptyDriver::stop(MachineId id) {
  kj::String name = kj::str(id);
  return gceCommand({"echo", name, "NEED-STOP"});
}

kj::Promise<void> EmptyDriver::gceCommand(kj::ArrayPtr<const kj::StringPtr> args,
                                        int stdin, int stdout) {
  auto fullArgs = kj::heapArrayBuilder<const kj::StringPtr>(args.size());
  fullArgs.addAll(args);
  sandstorm::Subprocess::Options options(fullArgs.finish());
  auto gceCommand = kj::strArray(options.argv, " ");
  KJ_LOG(INFO, gceCommand);
  options.stdin = stdin;
  options.stdout = stdout;
  return subprocessSet.waitForSuccess(kj::mv(options));
}

} // namespace blackrock


// Sandstorm Blackrock
// Copyright (c) 2014 Sandstorm Development Group, Inc.
// All Rights Reserved

#include "worker.h"
#include <sys/socket.h>
#include "nbd-bridge.h"
#include <sodium/randombytes.h>
#include <capnp/rpc-twoparty.h>
#include <sandstorm/version.h>
#include <sys/mount.h>
#include <grp.h>

namespace blackrock {

byte PackageMountSet::dummyByte = 0;

PackageMountSet::PackageMountSet(kj::AsyncIoContext& ioContext)
    : ioContext(ioContext) {}
PackageMountSet::~PackageMountSet() noexcept(false) {
  KJ_ASSERT(mounts.empty(), "PackageMountSet destroyed while packages still mounted!") { break; }
}

auto PackageMountSet::getPackage(PackageInfo::Reader package)
    -> kj::Promise<kj::Own<PackageMount>> {
  kj::Own<PackageMount> packageMount;

  auto id = package.getId();
  auto& slot = mounts[id];
  if (slot == nullptr) {
    // Create the NBD socketpair.
    int nbdSocketPair[2];
    KJ_SYSCALL(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, nbdSocketPair));
    kj::AutoCloseFd nbdKernelEnd(nbdSocketPair[0]);
    auto nbdUserEnd = ioContext.lowLevelProvider->wrapSocketFd(nbdSocketPair[1],
        kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP |
        kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC |
        kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK);

    packageMount = kj::heap<PackageMount>(*this, id,
        kj::str("/var/sandstorm/packages/", counter++), package.getVolume(),
        kj::mv(nbdUserEnd), kj::mv(nbdKernelEnd));
    slot = packageMount.get();
  } else {
    packageMount = kj::addRef(*slot);
  }

  auto promise = packageMount->loaded.addBranch();
  return promise.then([KJ_MVCAP(packageMount)]() mutable {
    return kj::mv(packageMount);
  });
}

PackageMountSet::PackageMount::PackageMount(PackageMountSet& mountSet,
    kj::ArrayPtr<const kj::byte> id, kj::String path, Volume::Client volume,
    kj::Own<kj::AsyncIoStream> nbdUserEnd, kj::AutoCloseFd nbdKernelEnd)
    : mountSet(mountSet),
      id(kj::heapArray(id)),
      path(kj::heapString(path)),
      volumeAdapter(kj::heap<NbdVolumeAdapter>(kj::mv(nbdUserEnd), kj::mv(volume))),
      nbdThread(mountSet.ioContext.provider->newPipeThread(
          [KJ_MVCAP(path), KJ_MVCAP(nbdKernelEnd)](
            kj::AsyncIoProvider& ioProvider,
            kj::AsyncIoStream& pipe,
            kj::WaitScope& waitScope) mutable {
        // Set up NBD.
        NbdDevice device;
        NbdBinding binding(device, kj::mv(nbdKernelEnd));
        Mount mount(device.getPath(), path, MS_RDONLY, nullptr);

        // Signal setup is complete.
        pipe.write(&dummyByte, 1).wait(waitScope);

        // Wait for pipe disconnect.
        while (pipe.tryRead(&dummyByte, 1, 1).wait(waitScope) > 0) {}

        // We'll close our end of the pipe on the way out, thus signaling completion.
      })),
      loaded(nbdThread.pipe->read(&dummyByte, 1).fork()) {}

PackageMountSet::PackageMount::~PackageMount() noexcept(false) {
  // We don't want to block waiting for the NBD thread to complete. So, we carefully wait for
  // the thread to report completion in a detached promise.
  //
  // TODO(cleanup): Make kj::Thread::detach() work correctly.

  mountSet.mounts.erase(id);

  auto pipe = kj::mv(nbdThread.pipe);
  pipe->shutdownWrite();
  loaded.addBranch().then([KJ_MVCAP(pipe)]() mutable {
    // OK, we read the first byte. Wait for the pipe to become readable again, signalling EOF
    // meaning the thread is done.
    auto promise = pipe->tryRead(&dummyByte, 1, 1).then([](size_t n) {
      KJ_ASSERT(n == 0, "expected EOF");
    });
    return promise.attach(kj::mv(pipe));
  }, [](kj::Exception&& e) {
    // Apparently the thread exited without ever signaling that NBD setup succeeded.
    return kj::READY_NOW;
  }).attach(kj::mv(nbdThread.thread))
    .detach([](kj::Exception&& exception) {
    KJ_LOG(ERROR, "Exception while trying to unmount package.", exception);
  });
}

// =======================================================================================

class WorkerImpl::SandstormCoreImpl: public sandstorm::SandstormCore::Server {
public:
  // TODO(someday): implement SandstormCore interface
};

class WorkerImpl::RunningGrain {
  // Encapsulates executing a grain on the worker.

public:
  RunningGrain(WorkerImpl& worker,
               Assignable<GrainState>::Client&& grainState,
               kj::Own<kj::AsyncIoStream> nbdSocket,
               Volume::Client volume,
               kj::Own<kj::AsyncIoStream> capnpSocket,
               kj::Own<PackageMountSet::PackageMount> packageMount,
               sandstorm::Subprocess::Options&& subprocessOptions,
               kj::PromiseFulfillerPair<sandstorm::Supervisor::Client> paf =
                   kj::newPromiseAndFulfiller<sandstorm::Supervisor::Client>())
      : worker(worker),
        grainState(kj::mv(grainState)),
        packageMount(kj::mv(packageMount)),
        nbdVolume(kj::mv(nbdSocket), kj::mv(volume)),
        process(kj::mv(subprocessOptions)),
        capnpSocket(kj::mv(capnpSocket)),
        vatNetwork(*this->capnpSocket, capnp::rpc::twoparty::Side::SERVER),
        rpcSystem(capnp::makeRpcServer(vatNetwork, kj::heap<SandstormCoreImpl>())),
        disconnectTask(vatNetwork.onDisconnect().attach(kj::defer([this]() {
          disconnected = true;
        })).eagerlyEvaluate(nullptr)),
        exitedFulfiller(kj::mv(paf.fulfiller)),
        exitedPromise(paf.promise.fork()) {
    randombytes(id, sizeof(id));
    worker.runningGrainsByPid[process.getPid()] = this;
  }

  ~RunningGrain() {
    if (process.isRunning()) {
      worker.runningGrainsByPid.erase(process.getPid());
    }
    worker.runningGrains.erase(id);
  }

  kj::ArrayPtr<const byte> getId() { return id; }

  sandstorm::Supervisor::Client getSupervisor() {
    if (disconnected) {
      // Presumably we'll get notification of exit shortly, but we shouldn't return a broken
      // supervisor just yet because we might still be cleaning up (unmounting) and it would be
      // bad if the coordinator decided to revoke our access to the volume. So, we return a promise
      // that will be broken as soon as we receive notification of exit.
      return exitedPromise.addBranch();
    } else {
      capnp::MallocMessageBuilder builder(8);
      auto root = builder.getRoot<capnp::rpc::twoparty::VatId>();
      root.setSide(capnp::rpc::twoparty::Side::CLIENT);
      return rpcSystem.bootstrap(root).castAs<sandstorm::Supervisor>();
    }
  }

  void exited(int status) {
    // Caller already erased us from worker.runningGrainsByPid.
    process.notifyExited(status);
    exitedFulfiller->reject(KJ_EXCEPTION(FAILED, "grain has shut down"));
  }

private:
  WorkerImpl& worker;
  byte id[16];
  Assignable<GrainState>::Client grainState;

  kj::Own<PackageMountSet::PackageMount> packageMount;
  NbdVolumeAdapter nbdVolume;
  sandstorm::Subprocess process;

  kj::Own<kj::AsyncIoStream> capnpSocket;
  capnp::TwoPartyVatNetwork vatNetwork;
  capnp::RpcSystem<capnp::rpc::twoparty::VatId> rpcSystem;
  // Cap'n Proto RPC connection to the grain's supervisor.

  bool disconnected = false;
  kj::Promise<void> disconnectTask;
  // `disconnected` is set true (by `disconnectTask`) when we receive notification of disconnect
  // from `vatNetwork`, indicating that the supervisor is shutting down.

  kj::Own<kj::PromiseFulfiller<sandstorm::Supervisor::Client>> exitedFulfiller;
  kj::ForkedPromise<sandstorm::Supervisor::Client> exitedPromise;
  // `exitedPromise` is rejected (via `exitedFulfiller`) when the process exits. This is used only
  // as a diversion in the rare case that a Coordinator tries to restart a process immediately
  // after exit.
};

WorkerImpl::WorkerImpl(kj::LowLevelAsyncIoProvider& ioProvider, PackageMountSet& packageMountSet)
    : ioProvider(ioProvider), packageMountSet(packageMountSet) {}

kj::Maybe<sandstorm::Supervisor::Client> WorkerImpl::getRunningGrain(kj::ArrayPtr<const byte> id) {
  auto iter = runningGrains.find(id);
  if (iter == runningGrains.end()) {
    return nullptr;
  } else {
    return iter->second->getSupervisor();
  }
}

bool WorkerImpl::childExited(pid_t pid, int status) {
  auto iter = runningGrainsByPid.find(pid);
  if (iter == runningGrainsByPid.end()) {
    return false;
  } else {
    RunningGrain* grain = iter->second;
    runningGrainsByPid.erase(iter);
    grain->exited(status);
    runningGrains.erase(grain->getId());
    return true;
  }
}

struct WorkerImpl::CommandInfo {
  kj::Array<kj::String> commandArgs;
  kj::Array<kj::String> envArgs;

  CommandInfo(sandstorm::spk::Manifest::Command::Reader command) {
    auto argvReader = command.getArgv();
    auto argvBuilder = kj::heapArrayBuilder<kj::String>(
        argvReader.size() + command.hasDeprecatedExecutablePath());
    if (command.hasDeprecatedExecutablePath()) {
      argvBuilder.add(kj::heapString(command.getDeprecatedExecutablePath()));
    }
    for (auto arg: argvReader) {
      argvBuilder.add(kj::heapString(arg));
    }
    commandArgs = argvBuilder.finish();

    envArgs = KJ_MAP(v, command.getEnviron()) {
      return kj::str("-e", v.getKey(), '=', v.getValue());
    };
  }
};

kj::Promise<void> WorkerImpl::newGrain(NewGrainContext context) {
  auto params = context.getParams();
  auto storageFactory = params.getStorage();

  auto grainVolume = storageFactory.newVolumeRequest().send().getVolume();

  auto supervisorPaf = kj::newPromiseAndFulfiller<sandstorm::Supervisor::Client>();

  auto req = storageFactory.newAssignableRequest<GrainState>();
  {
    auto grainStateValue = req.initInitialValue();
    grainStateValue.setActive(kj::mv(supervisorPaf.promise));
    grainStateValue.setVolume(grainVolume);
  }
  OwnedAssignable<GrainState>::Client grainState = req.send().getAssignable();

  sandstorm::Supervisor::Client supervisor = bootGrain(params.getPackage(), grainState,
      kj::mv(grainVolume), params.getCommand());

  supervisorPaf.fulfiller->fulfill(kj::cp(supervisor));

  auto results = context.getResults();
  results.setGrain(kj::mv(supervisor));
  results.setGrainState(kj::mv(grainState));

  // Promises are neat.
  return kj::READY_NOW;
}

kj::Promise<void> WorkerImpl::restoreGrain(RestoreGrainContext context) {
  auto params = context.getParams();

  context.getResults().setGrain(bootGrain(
      params.getPackage(), params.getGrainState(),
      params.getVolume(), params.getCommand()));

  return kj::READY_NOW;
}

sandstorm::Supervisor::Client WorkerImpl::bootGrain(PackageInfo::Reader packageInfo,
    Assignable<GrainState>::Client grainState, Volume::Client grainVolume,
    sandstorm::spk::Manifest::Command::Reader commandReader) {
  // Copy command info from params, since params will no longer be valid when we return.
  CommandInfo command(commandReader);

  // Make sure the package is mounted, then start the grain.
  return packageMountSet.getPackage(packageInfo)
      .then([this,grainState,KJ_MVCAP(command),KJ_MVCAP(grainVolume)](auto&& packageMount) mutable {
    // Create the NBD socketpair. The Supervisor will actually mount the NBD device (in its own
    // mount namespace) but we'll implement it in the Worker.
    int nbdSocketPair[2];
    KJ_SYSCALL(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, nbdSocketPair));
    kj::AutoCloseFd nbdKernelEnd(nbdSocketPair[0]);
    auto nbdUserEnd = ioProvider.wrapSocketFd(nbdSocketPair[1],
        kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP |
        kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC |
        kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK);

    // Create the Cap'n Proto socketpair, for Worker <-> Supervisor communication.
    int capnpSocketPair[2];
    KJ_SYSCALL(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, capnpSocketPair));
    kj::AutoCloseFd capnpSupervisorEnd(nbdSocketPair[0]);
    auto capnpWorkerEnd = ioProvider.wrapSocketFd(nbdSocketPair[1],
        kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP |
        kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC |
        kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK);

    // Build array of StringPtr for argv.
    KJ_STACK_ARRAY(kj::StringPtr, argv,
        command.commandArgs.size() + command.envArgs.size() + 4, 16, 16);
    {
      int i = 0;
      argv[i++] = "blackrock";
      argv[i++] = "grain";
      for (auto& envArg: command.envArgs) {
        argv[i++] = envArg;
      }
      argv[i++] = "--";
      argv[i++] = packageMount->getPath();
      for (auto& commandArg: command.commandArgs) {
        argv[i++] = commandArg;
      }
      KJ_ASSERT(i == argv.size());
    }

    // Build the subprocess options.
    sandstorm::Subprocess::Options options("/proc/self/exe");
    options.argv = argv;

    // Pass the capnp socket on FD 3 and the kernel end of the NBD socketpair as FD 4.
    int moreFds[2] = { capnpSupervisorEnd, nbdKernelEnd };
    options.moreFds = moreFds;

    // Make the RunningGrain.
    auto grain = kj::heap<RunningGrain>(
        *this, kj::mv(grainState), kj::mv(nbdUserEnd), kj::mv(grainVolume),
        kj::mv(capnpWorkerEnd), kj::mv(packageMount), kj::mv(options));

    auto supervisor = grain->getSupervisor();
    auto id = grain->getId();

    // Put it in the map so that it doesn't go away and can be restore()ed.
    runningGrains[id] = kj::mv(grain);

    return supervisor;
  });
}

// =======================================================================================

class SupervisorMain::SystemConnectorImpl: public sandstorm::SupervisorMain::SystemConnector {
public:
  RunResult run(kj::AsyncIoContext& ioContext,
                sandstorm::Supervisor::Client mainCapability) const override {
    auto runner = kj::heap<Runner>(ioContext, kj::mv(mainCapability));

    capnp::MallocMessageBuilder message(8);
    auto vatId = message.getRoot<capnp::rpc::twoparty::VatId>();
    vatId.setSide(capnp::rpc::twoparty::Side::SERVER);
    auto core = runner->rpcSystem.bootstrap(vatId).castAs<sandstorm::SandstormCore>();

    auto promise = runner->network.onDisconnect();

    return { promise.attach(kj::mv(runner)), kj::mv(core) };
  }

  void checkIfAlreadyRunning() const override {
    // Not relevant for Blackrock.
  }

private:
  struct Runner {
    kj::Own<kj::AsyncIoStream> stream;
    capnp::TwoPartyVatNetwork network;
    capnp::RpcSystem<capnp::rpc::twoparty::VatId> rpcSystem;

    Runner(kj::AsyncIoContext& ioContext, sandstorm::Supervisor::Client mainCapability)
        : stream(ioContext.lowLevelProvider->wrapSocketFd(3,
              kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP |
              kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC |
              kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK)),
          network(*stream, capnp::rpc::twoparty::Side::CLIENT),
          rpcSystem(network, kj::mv(mainCapability)) {}
  };
};

SupervisorMain::SupervisorMain(kj::ProcessContext& context)
    : context(context),
      sandstormSupervisor(context) {}

kj::MainFunc SupervisorMain::getMain() {
  return kj::MainBuilder(context, "Blackrock version " SANDSTORM_VERSION,
                         "Runs a Blackrock grain supervisor for an instance of the package found "
                         "at path <package>. <command> is executed inside the sandbox to start "
                         "the grain. The caller must provide a Cap'n Proto towparty socket on "
                         "FD 3 which is used to talk to the grain supervisor, and FD 4 must be "
                         "a socket implementing the NBD protocol exporting the grain's mutable "
                         "storage.\n"
                         "\n"
                         "NOT FOR HUMAN CONSUMPTION: Given the FD requirements, you obviously "
                         "can't run this directly from the command-line. It is intended to be "
                         "invoked by the Blackrock worker.")
      .addOptionWithArg({'e', "env"}, KJ_BIND_METHOD(sandstormSupervisor, addEnv), "<name>=<val>",
                        "Set the environment variable <name> to <val> inside the sandbox.  Note "
                        "that *no* environment variables are set by default.")
      .expectArg("<package>", KJ_BIND_METHOD(sandstormSupervisor, setPkg))
      .expectOneOrMoreArgs("<command>", KJ_BIND_METHOD(sandstormSupervisor, addCommandArg))
      .callAfterParsing(KJ_BIND_METHOD(*this, run))
      .build();
}

kj::MainBuilder::Validity SupervisorMain::run() {
  sandstormSupervisor.setAppName("appname-not-applicable");
  sandstormSupervisor.setGrainId("grainid-not-applicable");

  SystemConnectorImpl connector;
  sandstormSupervisor.setSystemConnector(connector);

  // Set CLOEXEC on the two special FDs we inherited, to be safe.
  KJ_SYSCALL(fcntl(3, F_SETFD, FD_CLOEXEC));
  KJ_SYSCALL(fcntl(4, F_SETFD, FD_CLOEXEC));

  // Enter mount namespace!
  KJ_SYSCALL(unshare(CLONE_NEWNS));

  // We'll mount our grain data on /mnt because it's our own mount namespace so why not?
  NbdDevice device;
  NbdBinding binding(device, kj::AutoCloseFd(4));
  Mount mount(device.getPath(), "/mnt", 0, nullptr);
  sandstormSupervisor.setVar("/mnt");
  KJ_SYSCALL(chown("/mnt", 1000, 1000));

  // In order to ensure we get a chance to clean up after the supervisor exits, we run it in a
  // fork.
  sandstorm::Subprocess child([this]() {
    // Unfortunately, we are still root here. Drop privs.
    KJ_SYSCALL(setgroups(0, nullptr));
    KJ_SYSCALL(setresgid(1000, 1000, 1000));
    KJ_SYSCALL(setresuid(1000, 1000, 1000));

    // Make sure the `sandbox` directory exists.
    mkdir("/mnt/sandbox", 0777);

    KJ_IF_MAYBE(error, sandstormSupervisor.run().getError()) {
      KJ_LOG(ERROR, *error);
      return 1;
    } else {
      KJ_LOG(ERROR, "sandstorm::SupervisorMain::run() should not have returned");
      return 1;
    }
  });

  // FD 3 is used by the child. Close our handle.
  KJ_SYSCALL(close(3));

  child.waitForSuccess();
  return true;
}

}  // namespace blackrock
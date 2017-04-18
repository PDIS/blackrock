@0xa9101b1fec595220;

using import "/blackrock/master.capnp".MasterConfig;

const empty :MasterConfig = (
  workerCount = 1,
  frontendCount = 1,
  frontendConfig = (
    baseUrl = "http://pdis.rocks",
    wildcardHost = "*.pdis.rocks",
    allowDemoAccounts = true,
    isQuotaEnabled = false,
    isTesting = true,
    outOfBeta = true,
    allowUninvited = true,
    replicasPerMachine = 2
  ),
  empty = ()
);


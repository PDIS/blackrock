@0xa9101b1fec595220;
using import "/blackrock/master.capnp".MasterConfig;
const gce :MasterConfig = (
  workerCount = 1,
  frontendCount = 1,
  frontendConfig = (
    baseUrl = "http://pdis.rocks",
    wildcardHost = "*.pdis.rocks",
    allowDemoAccounts = true,
    isTesting = true,
    outOfBeta = true,
    allowUninvited = true,
    replicasPerMachine = 2
  ),
  
  gce = (
        project = "blackrock-mark", 
        zone = "asia-east1-a",
        instanceTypes = (
                storage = "n1-standard-1",
                worker = "n1-standard-1",
                frontend = "g1-small",
                mongo = "g1-small"
        )
  )
);

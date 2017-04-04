#! /bin/bash

set -euo pipefail

GCE_PROJECT=blackrock-mark
export CLOUDSDK_COMPUTE_ZONE=asia-east1-a

DRY_RUN=no
CONFIRM_EACH=no
HOTFIX=no

gce() {
  gcloud --project=$GCE_PROJECT compute "$@"
}

doit() {
  local ANSWER
  if [ "$CONFIRM_EACH" != "no" ]; then
    printf "\033[0;33m=== RUN? %s ===\033[0m" "$*"
    read -sn 1 ANSWER
    if [ -z "$ANSWER" ]; then
      printf "\r\033[K"
    else
      printf "\033[0;31m\r=== SKIPPED: %s ===\033[0m\n" "$*"
      return
    fi
  fi

  printf "\033[0;35m=== %s ===\033[0m\n" "$*"

  if [ "$DRY_RUN" = "no" ]; then
    "$@"
  fi
}

# Create a new image.
doit gce instances create build --image debian-8
doit sleep 10 # make sure instance is up
doit gce ssh build 'sudo sed -i -e "s/PermitRootLogin no/PermitRootLogin without-password/g" /etc/ssh/sshd_config; sudo service ssh restart'
doit gce copy-files blackrock.tar.xz root@build:/
doit gce ssh root@build --command "cd / && tar Jxof blackrock.tar.xz && rm /blackrock.tar.xz"
doit gce instances delete build -q --keep-disks boot
doit gce images create blackrock --source-disk build
doit gce disks delete -q build

# Also upload to master.
# mv bin/blackrock bin/blackrock-$BUILDSTAMP
# doit gce copy-files bin/blackrock root@master:/blackrock/bin/blackrock-$BUILDSTAMP

<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# Debian test guest

The second guest of `runbooks/qnx-probe-launch.sh`. The runbook expects
the disk image at `vms/debian-ivi-guest.qcow2` (or wherever
`DEBIAN_IMAGE` points) and logs in over SSH as `debian` with your key.

```bash
mkdir -p vms
# Debian 12 generic cloud image (amd64) from https://cloud.debian.org/images/cloud/
qemu-img create -f qcow2 -F qcow2 \
    -b /path/to/debian-12-generic-amd64.qcow2 vms/debian-ivi-guest.qcow2 10G

# Put your public key into the template, then build the seed image
sed "s|REPLACE_WITH_YOUR_SSH_PUBLIC_KEY|$(cat ~/.ssh/id_ed25519.pub)|" \
    guest/debian/user-data.template > guest/debian/user-data
cloud-localds vms/seed.iso guest/debian/user-data guest/debian/meta-data
```

Boot it once with the seed attached so cloud-init creates the user and
installs Xorg:

```bash
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 2G \
    -drive file=vms/debian-ivi-guest.qcow2,if=virtio,format=qcow2 \
    -drive file=vms/seed.iso,format=raw,if=virtio \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=net0 -nographic
```

SSH is forwarded to `127.0.0.1:2222` only. `cloud-localds` is in the
`cloud-image-utils` package. This preparation sequence has not yet been
run end to end from a fresh image.

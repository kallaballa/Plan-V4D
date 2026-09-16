#!/bin/bash
set -euo pipefail

# qemu-test.sh — Install + smoke-test the OBS-built Plan-V4D packages in QEMU VMs
#
# Uses the binaries already pulled into obs/results/<TARGET> and boots a real
# VM of each target distro with QEMU. x86_64 targets use KVM; the arm64
# (Raspbian_12) target runs under TCG emulation. Cloud-init is used to seed an
# ssh key + root password, then SSH drives the install + tests.
#
# Usage:
#   ./qemu-test.sh <target>     # tumbleweed | fedora | ubuntu | raspbian
#   ./qemu-test.sh all
#   ./qemu-test.sh --stop <target>   # power off a running test VM
#   ./qemu-test.sh --list-images     # show which base images are available

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/results"
WORK_ROOT="${QEMU_WORK_ROOT:-/tmp/opencode/qemu}"
IMG_DIR="$WORK_ROOT/images"
EFI="$IMG_DIR/QEMU_EFI.fd"
KEY="$SCRIPT_DIR/qemu-test/id_ed25519"

SSH_OPTS=(-o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR -i "$KEY")

# ---- Target configuration -----------------------------------------------
declare -A OS=(
    [tumbleweed]="image=tw.qcow2|arch=x86_64|port=2222"
    [fedora]="image=fedora.qcow2|arch=x86_64|port=2223"
    [ubuntu]="image=ubuntu.img|arch=x86_64|port=2224"
    [raspbian]="image=debian-arm64.qcow2|arch=aarch64|port=2225"
)

image_of() { get_kv "$1" image; }
arch_of()  { get_kv "$1" arch; }
port_of()  { get_kv "$1" port; }
get_kv() {
    local target="$1" key="$2"
    echo "${OS[$target]}" | tr '|' '\n' | sed -n "s/^$key=//p"
}

declare -A PKG_GLOB=(
    [tumbleweed]="plan-v4d-*.rpm"
    [fedora]="plan-v4d-*.rpm"
    [ubuntu]="plan-v4d-*.deb"
    [raspbian]="plan-v4d-*.deb"
)

declare -A RESULTS_DIR_MAP=(
    [tumbleweed]="openSUSE_Tumbleweed"
    [fedora]="Fedora"
    [ubuntu]="Ubuntu_24.04"
    [raspbian]="Raspbian_12"
)

PIDFILE_DIR="$WORK_ROOT/pids"
mkdir -p "$WORK_ROOT" "$IMG_DIR" "$PIDFILE_DIR" "$SCRIPT_DIR/qemu-test"

# ---- SSH key -------------------------------------------------------------
ensure_key() {
    [[ -f "$KEY" ]] || ssh-keygen -q -t ed25519 -N "" -f "$KEY"
}

# ---- Ensure base image ---------------------------------------------------
ensure_image() {
    local target="$1" img image
    image=$(image_of "$target")
    if [[ -f "$IMG_DIR/$image" ]]; then
        echo "  image: $IMG_DIR/$image"
    else
        echo "ERROR: missing image $IMG_DIR/$image for $target" >&2
        exit 1
    fi
}

# ---- QEMU boot -----------------------------------------------------------
launch_vm() {
    local target="$1"
    local image arch port user_data seed workdir overlay log pidfile
    image=$(image_of "$target")
    arch=$(arch_of "$target")
    port=$(port_of "$target")
    workdir="$WORK_ROOT/$target"
    mkdir -p "$workdir"
    overlay="$workdir/overlay.qcow2"
    seed="$workdir/seed.iso"
    log="$workdir/serial.log"
    pidfile="$PIDFILE_DIR/$target.pid"

    echo "=== Preparing $target (arch=$arch, ssh port=$port) ==="

    # COW overlay so the base image stays pristine; give the disk some room.
    if [[ ! -f "$overlay" ]]; then
        qemu-img create -q -f qcow2 -F qcow2 -b "$IMG_DIR/$image" "$overlay"
        qemu-img resize -q "$overlay" 8G 2>/dev/null || true
    fi

    user_data="$workdir/user-data"
    cat > "$user_data" <<EOF
#cloud-config
password: v4dtest
chpasswd: { expire: False }
ssh_pwauth: true
disable_root: false
runcmd:
  - [ sh, -c, 'echo force > /var/lib/cloud/instance/growroot' ]
EOF
    cat >> "$user_data" <<EOF

ssh_authorized_keys:
  - $(cat "$KEY.pub")
EOF
    cat > "$workdir/meta-data" <<EOF
instance-id: v4d-test-${target}-$(date +%s)
local-hostname: v4dtest-${target}
EOF

    (cd "$workdir" && mkisofs -quiet -r -l -V cidata -o "$seed" user-data meta-data)

    local common=( -smp 4 -m 4096 -pidfile "$pidfile" -display none -daemonize )
    local net=( -netdev "user,id=n0,hostfwd=tcp::${port}-:22" -device virtio-net-pci,netdev=n0 )
    local disk=( -drive "file=$overlay,if=virtio,format=qcow2" )
    # Seed attachment differs per target:
    #  - Tumbleweed/Fedora: IDE cdrom (-cdrom) exposes /dev/sr0 -> NoCloud picks it up.
    #  - Ubuntu/Raspbian minimal images: kernel lacks IDE cdrom driver, so attach the
    #    same ISO as a virtio-blk disk so it appears as /dev/vdb (label cidata).
    if [[ "$target" == "ubuntu" || "$target" == "raspbian" ]]; then
        local seeddev=( -drive "file=$seed,if=virtio,format=raw" )
    else
        local seeddev=( -cdrom "$seed" )
    fi

    case "$arch" in
        x86_64)
            qemu-system-x86_64 "${common[@]}" \
                -machine q35 -accel kvm -cpu host \
                -serial "file:$log" \
                "${net[@]}" "${disk[@]}" "${seeddev[@]}"
            ;;
        aarch64)
            qemu-system-aarch64 "${common[@]}" \
                -machine virt -cpu max -bios "$EFI" \
                -serial "file:$log" \
                "${net[@]}" "${disk[@]}" "${seeddev[@]}"
            ;;
    esac

    # Wait for ssh to come up.
    local tries=0
    echo "  waiting for ssh on 127.0.0.1:$port ..."
    until ssh "${SSH_OPTS[@]}" -p "$port" root@127.0.0.1 true 2>/dev/null; do
        tries=$((tries + 1))
        if (( tries > 60 )); then
            echo "ERROR: $target did not come up via ssh (see $log)" >&2
            tail -30 "$log" >&2 || true
            kill "$(cat "$pidfile")" 2>/dev/null || true
            exit 1
        fi
        sleep 5
    done
    echo "  ssh up."
}

stop_vm() {
    local target="$1"
    local pidfile="$PIDFILE_DIR/$target.pid"
    local port pid
    port=$(port_of "$target")
    if [[ -f "$pidfile" ]]; then
        pid=$(cat "$pidfile")
        ssh "${SSH_OPTS[@]}" -p "$port" root@127.0.0.1 poweroff 2>/dev/null || true
        sleep 8
        kill "$pid" 2>/dev/null || true
        rm -f "$pidfile"
        echo "  $target stopped."
    fi
}

# ---- Run the remote tests ------------------------------------------------
run_tests() {
    local target="$1"
    local port pkgdir glob
    port=$(port_of "$target")
    pkgdir="$RESULTS_DIR/${RESULTS_DIR_MAP[$target]}"
    glob="${PKG_GLOB[$target]}"

    echo "=== Copying packages to $target ==="
    ssh "${SSH_OPTS[@]}" -p "$port" root@127.0.0.1 'mkdir -p /root/pkgs'
    scp "${SSH_OPTS[@]}" -P "$port" -q "$pkgdir"/$glob root@127.0.0.1:/root/pkgs/

    echo "=== Running tests in $target ==="
    ssh "${SSH_OPTS[@]}" -p "$port" root@127.0.0.1 TARGET="$target" 'bash -s' < "$SCRIPT_DIR/qemu-test/remote-test.sh"
    local rc=$?
    echo "=== $target remote test exit: $rc ==="
    return $rc
}

# ---- Main -----------------------------------------------------------------
TARGET="${1:-all}"
case "$TARGET" in
    --list-images)
        for t in tumbleweed fedora ubuntu raspbian; do
            printf '%-10s %s/%s\n' "$t" "$IMG_DIR" "$(image_of "$t")"
        done
        exit 0
        ;;
    --stop)
        stop_vm "${2:-}"
        exit 0
        ;;
esac

if [[ "$TARGET" == "all" ]]; then
    for t in tumbleweed fedora ubuntu raspbian; do
        ensure_key
        ensure_image "$t"
        launch_vm "$t"
        rc=0
        run_tests "$t" || rc=$?
        stop_vm "$t"
        echo ""
        echo "===================== $t: $([ $rc -eq 0 ] && echo PASS || echo FAIL) ====================="
        echo ""
        [[ $rc -eq 0 ]] || exit 1
    done
else
    ensure_key
    ensure_image "$TARGET"
    launch_vm "$TARGET"
    rc=0
    run_tests "$TARGET" || rc=$?
    stop_vm "$TARGET"
    exit $rc
fi
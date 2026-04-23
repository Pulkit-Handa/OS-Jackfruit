# Jackfruit: Multi-Container Supervised Runtime

## 1. Team Information
* **Name:** Pulkit Handa (PES1UG24AM208)
* **Name:** Piyush G Nadgir (PES1UG24AM190)

---

## 2. Comprehensive Setup & Execution

### 2.1 Environment Preparation (Kali Linux)
Since the project was developed on a rolling-release Kali distribution (Kernel 6.18+), specific manual environment patches were required:
```bash
# Install kernel-specific headers
sudo apt update && sudo apt install -y linux-headers-$(uname -r)

# Patch the environment check (Bypass hardcoded Ubuntu check)
sed -i 's/ID=kali/ID=ubuntu/' /etc/os-release
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### 2.2 Root Filesystem Setup
The containers use a lightweight Alpine Linux distribution for the root filesystem:
```bash
mkdir -p rootfs-base rootfs-alpha rootfs-beta
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-*.tar.gz -C rootfs-base

# Populate writable container roots with test binaries
cp -a ./rootfs-base/. ./rootfs-alpha/
cp -a ./rootfs-base/. ./rootfs-beta/
cp memory_hog cpu_hog io_pulse ./rootfs-alpha/
cp memory_hog cpu_hog io_pulse ./rootfs-beta/
```

### 2.3 Build and Load
```bash
# Compile with thread support
make clean && make

# Load the custom Kernel Monitor
sudo insmod monitor.ko

# Verify the device node exists
ls -l /dev/container_monitor
```

---

## 3. Demo Evidence & Screenshots

**Environment Preparation**
Preparing the Alpine root filesystem and populating it with statically linked test binaries.
![env_prep](screenshots/env_prep.png)

**Pre-flight Environment Check**
Evidence of the bypassed pre-flight script successfully verifying kernel header paths and namespace availability.
![env_check](screenshots/env_check.png)

**Supervisor Initialization**
The supervisor daemon initialized, binding to the Unix Domain Socket and opening the kernel device.
![alpha_super](screenshots/alpha_super.png)

**Container Run Execution**
CLI client successfully communicating with the supervisor to request a new isolated instance named 'alpha'.
![alpha](screenshots/alpha.png)

**PID Namespace Isolation**
Inside the container, running `ps` proves the process is isolated from the host and running as PID 1.
![pid_isolation](screenshots/pid_isolation.png)

**Host-Side Process Tracking**
The supervisor tracking the container via its global Host PID while the child remains jailed.
![pid_isolation_super](screenshots/pid_isolation_super.png)

**System Jail & Chroot Verification**
Demonstrating that the container is locked into the Alpine environment and cannot access the Kali host filesystem.
![sys_jail](screenshots/sys_jail.png)

**Kernel Hard Limit Enforcement**
Crucial evidence from `dmesg` showing the kernel module delivering a `SIGKILL` once the 50MiB hard limit was breached.
![sys_jail_super](screenshots/sys_jail_super.png)

**Multi-Container Concurrency**
Running the `beta` container instance with a 100MB workload alongside the active `alpha` instance.
![multi_alpha](screenshots/multi_alpha.png)
![multi_beta](screenshots/multi_beta.png)

**Supervisor Multi-Tenancy Logs**
The supervisor event loop managing multiple PIDs and distinct container configurations concurrently via IPC.
![multi_super](screenshots/multi_super.png)

---

## 4. Engineering Analysis

### 4.1 Isolation Logic
We implemented a **re-execution pattern**. The `engine` clones itself, and the child process executes a specific `child_fn` that configures the environment:
* **UTS Namespace:** Sets a private hostname (e.g., `alpha`).
* **Mount Namespace:** Performs a `chroot()` into the rootfs folder and mounts a fresh `/proc`. This ensures the container has its own view of the system state.
* **PID Namespace:** Ensures the child process cannot see or interfere with host-level processes.

### 4.2 IPC and Logging
* **Control Plane:** We chose **Unix Domain Sockets (`AF_UNIX`)** for communication between the CLI and the Supervisor. Sockets provide bidirectional, connection-oriented communication, which is superior to FIFOs for handling request-response cycles.
* **Bounded Buffer:** A ring buffer was implemented to handle container logs. We used a **Producer-Consumer model** where the supervisor reads from the container pipe (Producer) and a background thread writes to disk (Consumer). This prevents the supervisor from blocking if disk I/O is slow.

### 4.3 Kernel Module Monitoring
The monitor tracks the **Resident Set Size (RSS)**.
* **Mechanism:** Every 1 second, a kernel timer iterates through a linked list of registered PIDs.
* **Locking:** A **Mutex** protects this list. We chose a Mutex over a Spinlock because our IOCTL path performs operations that may sleep (like memory allocation).
* **Modern Kernel Compatibility:** For Linux Kernels 6.18+, we replaced the deprecated `del_timer_sync` with `timer_shutdown_sync` to ensure stable module unloading.

---

## 5. Design Tradeoffs
* **Polling vs. Event-based:** We used a 1-second timer for the monitor. While event-based monitoring (like `mmap` hooks) is more precise, the timer method provides a significantly lower CPU overhead for a demonstration runtime.
* **Static Binaries:** Test binaries (`memory_hog`) were compiled with `-static` to ensure they run inside the Alpine environment without needing host-side shared libraries.

---
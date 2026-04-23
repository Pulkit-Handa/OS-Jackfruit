# OS-Jackfruit

**Multi-Container Supervised Runtime with Kernel Memory Monitoring**

## Authors

- **Pulkit Handa** — PES1UG24AM208
- **Piyush G Nadgir** — PES1UG24AM190

---

## Overview

OS-Jackfruit is a Linux container runtime written in C. It combines:

1. a **long-running supervisor** that can manage multiple containers at once,
2. a **CLI client** that sends commands to the supervisor over a UNIX domain socket,
3. a **bounded-buffer logging pipeline** for container stdout/stderr, and
4. a **kernel module** that monitors container memory usage and enforces soft and hard limits.

The runtime creates isolated container processes using Linux namespaces and `chroot()`. Each container gets:

- its own **PID namespace**
- its own **UTS namespace**
- its own **mount namespace**
- a separate **root filesystem copy**
- a dedicated **log file**
- optional **soft/hard memory limits**
- optional **nice** value for scheduling experiments

---

## Repository Layout

```text
boilerplate/
├── engine.c          # user-space runtime and CLI
├── monitor.c         # kernel module for memory monitoring
├── monitor_ioctl.h   # ioctl interface shared by user-space and kernel-space
├── cpu_hog.c         # CPU-bound workload
├── io_pulse.c        # I/O-bound workload
├── memory_hog.c      # memory-heavy workload
├── Makefile          # build targets
└── environment-check.sh
```

Additional files in the repository include:

- `project-guide.md` — detailed implementation requirements and task breakdown
- `run_all.sh` — helper script
- `test.sh` — test helper
- `screenshots/` — demo evidence
- `SCREENSHOTS.pdf` — compiled screenshot set

---

## Features

### User-space runtime
- Starts a persistent **supervisor** process
- Supports multiple containers concurrently
- Tracks container metadata:
  - container ID
  - host PID
  - start time
  - current state
  - memory limits
  - exit status / exit signal
  - log file path
- Uses a **UNIX domain socket** at `/tmp/mini_runtime.sock` for CLI-to-supervisor control
- Captures container output through a **pipe + producer/consumer buffer**
- Reaps children and avoids zombie processes

### Kernel-space memory monitor
- Exposes `/dev/container_monitor`
- Registers and unregisters PIDs through `ioctl`
- Stores monitored processes in a linked list
- Checks RSS periodically
- Emits a soft-limit warning once
- Sends `SIGKILL` when the hard limit is exceeded
- Removes stale entries when processes exit

---

## Build Requirements

The project is intended for a Linux system with:

- `gcc`
- `make`
- `build-essential`
- Linux kernel headers matching the running kernel
- permission to load kernel modules

Install the common dependencies on Ubuntu-based systems:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

## Build

From the `boilerplate/` directory:

```bash
make
```

This builds:

- `engine`
- `memory_hog`
- `cpu_hog`
- `io_pulse`
- `monitor.ko`

For a CI-style build of only the user-space binaries:

```bash
make ci
```

To clean build artifacts:

```bash
make clean
```

---

## Runtime Setup

### 1. Prepare a root filesystem

The runtime expects each live container to use its own writable rootfs directory.

Example:

```bash
mkdir -p rootfs-base rootfs-alpha rootfs-beta
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base/. rootfs-alpha/
cp -a rootfs-base/. rootfs-beta/
```

Copy any workload binaries into the container rootfs you plan to run them in:

```bash
cp memory_hog cpu_hog io_pulse rootfs-alpha/
cp memory_hog cpu_hog io_pulse rootfs-beta/
```

### 2. Check the environment

```bash
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### 3. Load the kernel module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### 4. Start the supervisor

```bash
./engine supervisor ./rootfs-base
```

The `base-rootfs` argument is accepted by the supervisor, while each container is launched from its own writable rootfs directory.

---

## CLI Usage

The command interface is fixed by the implementation in `engine.c`:

```bash
./engine supervisor <base-rootfs>
./engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
./engine run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
./engine ps
./engine logs <id>
./engine stop <id>
```

### Command behavior

#### `supervisor`
Starts the long-running supervisor. It listens on:

```bash
/tmp/mini_runtime.sock
```

#### `start`
Launches a container in the background and returns after the supervisor accepts it.

Example:

```bash
./engine start alpha ./rootfs-alpha /bin/sh
```

#### `run`
Launches a container and waits for it to exit. The client returns the container exit status.

Example:

```bash
./engine run beta ./rootfs-beta /bin/sh
```

#### `ps`
Prints tracked container metadata.

Example:

```bash
./engine ps
```

#### `logs`
Streams the saved log file for a container.

Example:

```bash
./engine logs alpha
```

#### `stop`
Requests termination of a running container.

Example:

```bash
./engine stop alpha
```

---

## Memory Limit Flags

Both `start` and `run` accept optional limits:

- `--soft-mib N`
- `--hard-mib N`
- `--nice N`

Defaults used by the code:

- soft limit: **40 MiB**
- hard limit: **64 MiB**

Rules enforced by the parser:

- soft limit cannot exceed hard limit
- `--nice` must be between `-20` and `19`

Example:

```bash
./engine start alpha ./rootfs-alpha ./memory_hog --soft-mib 32 --hard-mib 48 --nice 5
```

---

## Isolation Model

When the supervisor starts a container, it uses `clone()` with:

- `CLONE_NEWPID`
- `CLONE_NEWUTS`
- `CLONE_NEWNS`

Inside the child process:

1. stdout/stderr are redirected to a pipe
2. hostname is set to the container ID
3. `nice()` is applied if requested
4. `chroot()` switches the process into the container rootfs
5. `/proc` is mounted
6. `execve()` launches the requested command

This gives each container its own process view and filesystem view.

---

## Logging Design

Container output is handled by a producer/consumer pipeline:

- the container writes to a pipe
- a producer thread reads from that pipe
- logs are pushed into a bounded shared buffer
- a consumer thread writes the log data to `logs/<container-id>.log`

Why this structure matters:

- it avoids blocking the supervisor on direct file writes
- it reduces the chance of deadlock when output is heavy
- it keeps per-container output persistent after exit

---

## Kernel Memory Monitoring

The kernel module in `monitor.c`:

- creates the character device `/dev/container_monitor`
- registers containers through `MONITOR_REGISTER`
- removes containers through `MONITOR_UNREGISTER`
- keeps a linked list of monitored processes
- checks RSS once per second
- logs a warning when the soft limit is exceeded
- sends `SIGKILL` when the hard limit is exceeded

The shared ioctl structure is defined in `monitor_ioctl.h`:

- `pid`
- `soft_limit_bytes`
- `hard_limit_bytes`
- `container_id`

---

## Notes

- The supervisor can run even if `/dev/container_monitor` is not present, but memory monitoring will be disabled.
- Each live container should use its own writable rootfs directory.
- The runtime writes logs to the `logs/` directory.
- The supervisor cleans up the socket file when it exits.

---

## Example Workflow

```bash
# Build
make

# Load monitor
sudo insmod monitor.ko

# Prepare rootfs copies
mkdir -p rootfs-base rootfs-alpha
cp -a rootfs-base/. rootfs-alpha/
cp memory_hog rootfs-alpha/

# Start supervisor
./engine supervisor ./rootfs-base

# In another terminal
./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 48
./engine ps
./engine logs alpha
./engine stop alpha
```

---

## License

GPL-licensed kernel module and accompanying user-space runtime code as included in the repository.
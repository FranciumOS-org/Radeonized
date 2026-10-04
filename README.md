# RembrandtGPU

Work towards a macOS driver for the AMD Radeon 680M (Rembrandt, `1002:1681`).

Today it contains one thing: **RembrandtProbe**, a read-only kext that reads the
GPU's IP discovery table and the state of its security processor (PSP), power
management firmware (SMU) and memory hub, without changing anything on the GPU.

```
make kext tools        # builds build/out/RembrandtProbe.kext, runs the host tests
sudo tools/load.sh     # loads it, prints the report (logs/<timestamp>/)
sudo tools/unload.sh
```

See docs/PLAN.md for the plan and CLAUDE.md for the rules the code follows.
Code is MIT; files in docs/linux-ref are from Linux (MIT, AMD).

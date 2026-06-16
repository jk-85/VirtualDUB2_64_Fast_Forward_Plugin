# Fast Scan 500 / 2000 / 3000 fps toolbar plug-in for VirtualDub2

Move fast forward in your video. You also can click on the input or
output pane to start or stop the fast forward.

## Build

1. Open `FastScan500.sln` in Visual Studio 2022.
2. Select `Release | x64`.
3. Build the solution.
4. Close VirtualDub2 completely.
5. Remove the previous `FastScan500.vdplugin` from `plugins64`.
6. Copy `bin\Release\FastScan500.vdplugin` to `plugins64`.
-> Step 6 will be tried automatically via Visual Studio after successful build.
   It will be copied into the path "C:\Program Files\VirtualDUB2\plugins64\".
7. Restart VirtualDub2.
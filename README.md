# Fast Scan 500 / 2000 / 3000 fps toolbar plug-in for VirtualDub2

Add additional buttons to your VirtualDUB2:
Move fast forward in your video with one click in three different speeds.
You also can click on the input or output pane to start or stop the fast forward process.

<img width="271" height="154" alt="pic1" src="https://github.com/user-attachments/assets/53f11e55-897c-4a18-82b9-e2eca131e9b8" />
<br>
<img width="432" height="301" alt="pic2" src="https://github.com/user-attachments/assets/4a61ffe6-ed7d-4f2e-b3a7-9bb81c8c91b1" />

## Build

1. Open `FastScan500.sln` in Visual Studio 2022.
2. Select `Release | x64`.
3. Build the solution.
4. Close VirtualDub2 completely.
5. Remove the previous `FastScan500.vdplugin` from `plugins64`.
6. Copy `bin\Release\FastScan500.vdplugin` to `plugins64`.

Step 6 will be tried automatically via Visual Studio after successful build.
It will be copied into the path "C:\Program Files\VirtualDUB2\plugins64\".

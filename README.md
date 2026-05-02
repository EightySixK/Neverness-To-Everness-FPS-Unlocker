# Neverness-To-Everness-FPS-Unlocker
Framerate unlocker for Neverness To Everness (NTE) <br>
Removes the FPS cap from the game, applying a user defined cap instead. <br>
This is done WITHOUT the usage of any sort of Frame "generation" technologies <br>
Also now featuring the option to disable the forced mouse smoothing that was applied to the third person camera.

## Installation <br>
<br>
Copy "version.dll" to: <br>
"C:\Program Files\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64"

Optional:
In the same directory, download the version_config.ini from /bin/ to choose a custom cap and disable mouse smoothing. 


## Uninstallation
Literally just delete "version.dll" from the games directory. <br>


# Build<br>
Requires MSVC 2022 + CMake<br>
Open up Terminal <br>
mkdir build && cd build <br>
cmake .. -G "Visual Studio 17 2022" -A x64 <br>
cmake --build . --config Release --target version_proxy <br>


I have not been banned for the usage of this unlocker. <br>
However it is still use at your own risk, I am not responsible for your account. 

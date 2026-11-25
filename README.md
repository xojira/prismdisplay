# PrismDisplay

PrismDisplay contains a Windows-native C++ core for desktop capture,
hardware H.264 encoding, and synthetic pen/touch input.

## WPF application

The .NET 8 WPF host project is
[src/app/PrismDisplay.csproj](./src/app/PrismDisplay.csproj).
The application entry is
[src/app/App.xaml](./src/app/App.xaml).
The host networking and native capture service is
[src/app/PrismHostService.cs](./src/app/PrismHostService.cs).

## Android client

The Android application module configuration is
[android/app/build.gradle.kts](./android/app/build.gradle.kts).
Its manifest is
[android/app/src/main/AndroidManifest.xml](./android/app/src/main/AndroidManifest.xml).
The Android Compose client is
[android/app/src/main/java/com/prismdisplay/client/MainActivity.kt](./android/app/src/main/java/com/prismdisplay/client/MainActivity.kt).
Wi-Fi TLS uses normal certificate validation; the server must present a certificate trusted by Android and valid for the host address.

## Native core

The native source is [src/native/PrismCore.cpp](./src/native/PrismCore.cpp).
Build it from the repository root in an x64 Native Tools Command Prompt:

```bat
cl /LD /O2 /std:c++20 src/native/PrismCore.cpp d3d11.lib dxgi.lib mfplat.lib mfuuid.lib mfreadwrite.lib strmiids.lib user32.lib
```

## Windows host packaging

From the repository root, build and publish the WPF host, then place the native
DLL beside the published executable:

```bat
dotnet publish src/app/PrismDisplay.csproj -c Release -r win-x64 --self-contained -o build_out
copy PrismCore.dll build_out\
```

For USB ADB tunneling, stage the Android platform-tools binaries as well. This
assumes the Android SDK is installed at the default per-user Windows location:

```bat
copy "%LocalAppData%\Android\Sdk\platform-tools\adb.exe" build_out\
copy "%LocalAppData%\Android\Sdk\platform-tools\AdbWinApi.dll" build_out\
copy "%LocalAppData%\Android\Sdk\platform-tools\AdbWinUsbApi.dll" build_out\
```

The automated Windows build is defined in
[.github/workflows/build-windows.yml](./.github/workflows/build-windows.yml).
When the workflow finishes, download the `PrismDisplay-win-x64` artifact from
the Actions tab and extract it on your Windows PC. To use USB ADB tunneling,
copy `adb.exe`, `AdbWinApi.dll`, and `AdbWinUsbApi.dll` from Android SDK
platform-tools beside `PrismDisplay.exe`. Then run `PrismDisplay.exe`.

The Android files currently include the app module configuration, manifest, and
activity, but not the root Gradle settings/build files or Gradle wrapper needed
to build the Android client.

## Indirect display driver

The KMDF/IddCx driver source is
[src/driver/PrismIndirectDisplay.cpp](./src/driver/PrismIndirectDisplay.cpp).
The package manifest is
[src/driver/PrismVirtualDisplay.inf](./src/driver/PrismVirtualDisplay.inf).
Build it in a Windows driver project with Visual Studio, the Windows Driver Kit,
and KMDF. Driver installation requires test signing or a properly signed release
package; it should not be installed on a normal machine from unverified source.

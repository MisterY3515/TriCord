# TriCord Revanced

## Warning
> This program is for now under developer, so until the 0.1.0 (and after) it can have issues and crash any time.

![License](https://img.shields.io/badge/license-GPLv3-green)
![Platform](https://img.shields.io/badge/platform-Nintendo%203DS-red)
![Downloads](https://img.shields.io/github/downloads/MisterY3515/TriCord-Revanced/total?style=flat&color=blue)

Discord client for Nintendo 3DS.

<img width="225" height="226" alt="image" src="https://github.com/user-attachments/assets/63bb4497-376b-4713-a03d-dc4c4219e541" />
Download QR for FBI.

This repository is a maintained fork of TriCord (made by 2b-zipper) focused on trying to add some features like the VC (just for the love of the game, I'm not even sure that is possible)

## Disclaimer

> **This project is unofficial and is provided for educational and experimental use.**
> It is not affiliated with or endorsed by Discord Inc.

This software is provided **"as is"**, and you use it at your own risk. The use of this application is entirely the user's own responsibility. The developers assume no responsibility for:
* Any damages, data loss, or account-related issues.
* Violations of Discord's **Terms of Service (ToS)** resulting from the use of this software.

## Screenshots
![login](screenshots/1.png)
![serverlist](screenshots/2.png)
![hamburger](screenshots/3.png)
![chat](screenshots/4.png)

## Features
- **Authentication**: QR code and Email/Password login. Multi-account support.
- **Text Chat**: Send and receive messages, emojis, attachments, embeds, reactions, and replies.
- **Voice Chat**: Join Voice Channels, speak, listen, and manage your microphone (Experimental).
- **Camera & Photos**: Take photos with the 3DS cameras and send them directly to chats.
- **Navigation**: Support for servers, channels, direct messages (DMs), and forum threads.
- **OTA Updates**: Built-in Updater to easily download and install new versions directly from the console.
- **Customization**: Customizable [themes](THEME_FORMAT.md) and Internationalization (8 languages).

## Current Status

- Text chat, guild navigation, forums, reactions, embeds, and multi-account support are implemented.
- Voice Chat is integrated and allows speaking and listening in Discord calls.
- Camera support allows taking live pictures and sending them as attachments.
- Built-in OTA Updater can install new releases automatically.
- Theme customization is supported via [THEME_FORMAT.md](THEME_FORMAT.md).
- Multiple UI languages are included: English, Italian, French, Spanish, German, Japanese, Polish, and Brazilian Portuguese.
- Persistent on-device logs and crash reports are available to help debug runtime failures.

## Installation

1. Download the latest build artifacts: `TriCord.3dsx` or `TriCord.cia`
2. Install via FBI if using the CIA build, or copy the 3DSX build into `/3ds/`
3. Launch the app and authenticate

## Login Methods

- **QR code**: scan the code from the Discord mobile app
- **Email / password**: direct login from the console UI
- **Token file**: create `sdmc:/3ds/TriCord/token.txt`, paste the token in plain text, then launch the app

Never share your Discord token or the local account storage files.

## Controls

- **Navigation**: D-Pad to move, `A` to select, `B` to go back, `Y` to enter Voice Channels
- **Message screen**: D-Pad to scroll, `X` for menu actions, `Y` for text input
- **Camera screen**: `A` to capture and send, `R` to flip front/back camera, `B` to cancel
- **General UI**: `SELECT` opens the hamburger menu, `L`+`R` toggles debug overlay
- **Voice actions**: `X` to toggle mute globally, `START` opens the voice screen when in a call, `L`+`B` to quickly leave the call

## Voice Chat

Voice chat is fully integrated and functional!
- Join is available from voice channels (Press `Y`).
- Quick mute toggle using the `X` button.
- Floating voice overlay allows you to manage the call even when browsing other servers.
- *Note: It is still experimental and might experience occasional stutters on older 3DS models.*

If the app crashes while joining voice, collect the files in `sdmc:/3ds/TriCord/`:
- `session.log`
- `session-prev.log`
- `crash_report.txt`
- `tricord.log`

## Configuration Paths

- Settings and local data: `sdmc:/3ds/TriCord/`
- Token import file: `sdmc:/3ds/TriCord/token.txt`
- Theme format reference: [THEME_FORMAT.md](THEME_FORMAT.md)

## Building From Source

### Prerequisites

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `devkitARM`
- devkitPro 3DS libraries used by the project
- `makerom` and `bannertool`
- `libsodium` for 3DS, built by the provided helper script

### Local Build

```bash
git clone https://github.com/MisterY3515/TriCord-Revanced.git
cd TriCord
make -j$(nproc)
```

Expected outputs:
- `TriCord.3dsx`
- `TriCord.cia`
- `TriCord.elf`

### GitHub Actions

The repository also contains a GitHub Actions workflow that builds the project automatically and uploads the generated artifacts.

## Libraries

### devkitPro / external libraries

- [libctru](https://github.com/devkitPro/libctru)
- [citro2d](https://github.com/devkitPro/citro2d)
- [citro3d](https://github.com/devkitPro/citro3d)
- [libcurl](https://curl.se/libcurl/)
- [mbed TLS](https://github.com/Mbed-TLS/mbedtls)
- [zlib](https://zlib.net/)
- [libopus](https://opus-codec.org/)
- [libsodium](https://libsodium.org/)

### Bundled libraries

- [RapidJSON](https://github.com/Tencent/rapidjson)
- [stb_image](https://github.com/nothings/stb)
- [qrcodegen](https://github.com/nayuki/QR-Code-generator)

### Additional resources

- CA bundle from [curl.se](https://curl.se/docs/caextract.html)
- [Twemoji](https://github.com/jdecked/twemoji)

## Known Issues

- Large accounts and large guilds can still stress RAM on old hardware
- Voice chat is experimental and may still crash or disconnect unexpectedly
- Discord API changes can break features without warning

## License

This project is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).

## Credits
- [MisterY3515](https://github.com/MisterY3515) for the main development of TriCord Revanced

## Original TriCord Credits
- [2b-zipper](https://github.com/2b-zipper) for the main development of the original TriCord
- [Str4ky](https://github.com/Str4ky) for the French translation
- [AverageJohtonian](https://github.com/AverageJohtonian) for the Spanish translation
- [RossoDev](https://github.com/RossoDev) for the Italian translation
- [MorrisTheGamer](https://github.com/MorrisTheGamer) for the German translation
- [ReisuErx](https://github.com/ReisuErx) for the Polish translation
- [wiretoscreen](https://github.com/wiretoscreen) for the Brazilian Portuguese translation
- [Discord Userdoccers](https://github.com/discord-userdoccers/discord-userdoccers) for the documentation of the Discord API
- And all other contributors!

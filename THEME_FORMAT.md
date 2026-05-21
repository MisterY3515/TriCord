# TriCord Theme Format Specification (Supported in v0.4.0+)

## Structure

Themes are JSON files stored in the following directory on your SD card:
`/3ds/TriCord/themes/`

### How to Install
1. Create or download a theme `.json` file.
2. Place it in the `/3ds/TriCord/themes/` folder.
3. Open TriCord and go to **Settings > Theme Manager**.
4. Select your theme and press **A** to apply.

## JSON Structure

```json
{
    "name": "My Theme",
    "author": "Author Name",
    "description": "Short description",
    "update_url": "https://raw.githubusercontent.com/user/repo/refs/heads/main/theme.json",
    "colors": {
        "ui": { ... },
        "text": { ... },
        "discord": { ... },
        "status": { ... }
    }
}
```

## Metadata

| Field | Required | Description |
| :--- | :---: | :--- |
| `name` | Yes | Display name shown in the theme manager. |
| `author` | No | Name of the theme creator. |
| `description` | No | Shown on the bottom screen when selected. |
| `update_url` | No | Link to the raw JSON file for updates (Y button). |

## Color Settings

All colors must be hex strings (`#RRGGBB` or `#RRGGBBAA`).
Omitted keys will automatically fall back to the values of the currently active base (Dark or Light mode). This allows you to create themes that define only accents while letting the app handle background switching.

### `ui` (UI Base)

| Key | Default | Description |
| :--- | :--- | :--- |
| `background` | `#313338` | Main background color. |
| `background_dark` | `#2B2D31` | Darker background (bottom screen, sidebars). |
| `background_light` | `#404249` | Lighter background (selection/hover highlights). |
| `accent` | `#5865F2` | Primary brand/accent color. |
| `selection` | `#5865F2` | Active selection and toggle switch background. |
| `separator` | `#949BA4` | 1px section divider lines. |
| `header_border` | `#FFFFFF1E` | Thin line below the top header. |
| `overlay` | `#00000096` | Semi-transparent modal backdrop. |
| `pure_white` | `#FFFFFF` | Toggle switch circles and other pure-white elements. |

### `text`

| Key | Default | Description |
| :--- | :--- | :--- |
| `main` | `#FFFFFF` | Primary text color. |
| `muted` | `#949BA4` | Secondary/dimmed text. |
| `link` | `#49BAFE` | Hyperlink color. |

### `discord` (Discord Elements)

| Key | Default | Description |
| :--- | :--- | :--- |
| `embed_bg` | `#2B2D31` | Background color for embed cards. |
| `embed_media_bg` | `#313338` | Background behind images/videos in embeds. |
| `reaction_bg` | `#404249` | Reaction button background. |
| `reaction_me_bg` | `#47648B` | Your own reaction button background. |
| `input_bg` | `#202225` | Text input field background. |

### `status`

| Key | Default | Description |
| :--- | :--- | :--- |
| `success` | `#43B16D` | Online indicator and success messages. |
| `error` | `#F0474D` | Destructive actions and error messages. |

## Example

You only need to specify the colors you want to change.

```json
{
    "name": "Red Accent",
    "colors": {
        "ui": {
            "accent": "#E74C3C",
            "selection": "#E74C3C"
        }
    }
}
```

## Full Template

A complete template with all color keys.

```json
{
    "name": "Theme Name",
    "author": "Author",
    "description": "Description",
    "update_url": "https://raw.githubusercontent.com/user/repo/refs/heads/main/theme.json",
    "colors": {
        "ui": {
            "background": "#313338",
            "background_dark": "#2B2D31",
            "background_light": "#404249",
            "accent": "#5865F2",
            "selection": "#5865F2",
            "separator": "#949BA4",
            "header_border": "#FFFFFF1E",
            "overlay": "#00000096",
            "pure_white": "#FFFFFF"
        },
        "text": {
            "main": "#FFFFFF",
            "muted": "#949BA4",
            "link": "#49BAFE"
        },
        "discord": {
            "embed_bg": "#2B2D31",
            "embed_media_bg": "#313338",
            "reaction_bg": "#404249",
            "reaction_me_bg": "#47648B",
            "input_bg": "#202225"
        },
        "status": {
            "success": "#43B16D",
            "error": "#F0474D"
        }
    }
}
```

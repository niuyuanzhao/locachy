# Appearance settings

In **Settings → Appearance → Colors**, choose **LOCachy Dark** or **LOCachy Light** and, separately, an **Accent color**. **Scheme default** follows the current scheme. A preset or custom `#RRGGBB` stays selected when the scheme changes. Wallpaper is independent of both.

The system stores the choices in `/etc/locachy/rc.conf`:

```sh
color_scheme="dark"       # dark or light
accent_color="default"    # default or #RRGGBB
```

An older configuration with neither key keeps the original dark appearance. Use Settings to apply changes to the running desktop. If you edit the file manually, `settings --apply` applies the saved settings; editing the file alone does not refresh all consumers.

- LOCachy windows and the shared desktop use the system scheme. UI colors change, not the original document, PDF annotation, screenshot, or a terminal application's explicit 24-bit color.
- labwc window decorations and menus use the same preferences. Each new LOCachy-managed foot window receives the new palette without rewriting `foot.ini`. Reopen existing foot windows to change their palette.
- Managed apps can read the scheme and accent from the Settings portal on their private session bus without access to the host configuration or bus. In Wayland tests, GTK 4.24 followed both preferences live; libadwaita 1.10 followed the scheme live and mapped the accent to its own named colors. Electron 42 followed the scheme live, including the web page's `prefers-color-scheme`, but did not demonstrate custom accent inheritance. GTK 3.24 and default Qt 5.15/6.12 Wayland widgets did not follow either preference. These are toolkit/version observations, not guarantees for every app. App-specific themes can override the system choice.

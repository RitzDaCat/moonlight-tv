// Optional KWin script that a Sunshine app entry loads for its session.
// KWin's focus stealing prevention keeps a native Wayland window started by the Sunshine service unfocused, so a
// fullscreen game stays under the panel and gets no controller input. This focuses the listed apps' windows as they open,
// moved first onto the streamed output: KWin opens a window on its active output, which can be the other monitor.
// Per-user KWin script settings. No games or monitor are selected by default.
const classes = String(readConfig("WindowClasses", "")).split(",").map(value => value.trim()).filter(Boolean);
const streamed = String(readConfig("StreamOutput", ""));

workspace.windowAdded.connect(added => {
    if (!classes.includes(added.resourceClass))
        return;
    const output = workspace.screens.find(o => o.name === streamed);
    if (output)
        workspace.sendClientToScreen(added, output);
    workspace.activeWindow = added;
});

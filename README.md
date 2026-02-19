# Mid Render
A lightweight render farm coordinator for small/medium VFX teams and freelancers

![Mid Render Image](docs/images/midrender_iyinJkrW2D.png)

Mid Render is a lightweight C++, Rust, and [Dear ImGUI](https://github.com/ocornut/imgui) render farm coordinator with a simple install process and automated connectivity. It's a self-healing mesh system where each render node can act as a coordinator. If one drops out, another takes its place. It uses UDP for fast handshakes but falls back to a file-system-based "phonebook" to help connect nodes in VPNs and complex network scenarios. Job coordination is over http. It's currently Windows-only, but will eventually expand to macOS and Linux.

It has a very simple job template setup that uses JSON templates to launch commands with flags, including regex hints for parsing stdout and tracking progress. It's DCC agnostic. Job templates and DCC submission plugins are included as examples. 

Each node has its own LiteDB database and collects snapshots of the current "leaders" state every 30 seconds. If the current leader drops out, a new leader takes over. The worst-case scenario: Frames rendered in the last 30 seconds will be re-rendered.

---

> [!NOTE]
> More advanced docs will come with later revisions, but here are the basics:

## Installation and Setup

Getting up and running is very simple. Download the latest `.exe` from releases and install on every node.

> [!WARNING]
> This installer opens up the HTTP port 8420 and the UDP port 4243 in the Windows Firewall. It also installs a shortcut in your startup folder.

In the settings panel, browse to, or paste a shared directory that every node can access. This directory can be an SMB share on a NAS or a shared folder in a file sync service like LucidLink, Dropbox, Synology Drive, Resilio, Syncthing, or others. This folder will contain the "phonebook" that helps connect nodes and all logs (Mid Render logs and DCC output logs). Press Save.

![Mid Render Image](docs/images/midrender_EQ0rnOsLs2.png)

### Advanced Setup

#### Tags

Tags are how you filter which DCCs each node can render. You can also influence the "Leader" election process. Tags are comma-separated like: `ae, blend, leader`.

![Mid Render Image](docs/images/midrender_8VMaMk0sEq.png)

##### Options:

- `ae` is the tag used for the provided Adobe After Effect job template. Using this tag will allow After Effects rendering on this node.
- `blend` is the tag used for the provided Blender job templates.
- `c4d` is the tag used for the provided Cinema 4d job templates.
- `leader` is a tag that forces leadership priority to a node. If this node is available, the mesh will recognize its leadership in coordination, but fall back to other nodes if it drops out.
- `noleader` signifies that you don't want this node to have leadership. It will only fall back to a leadership if it's the only node left alive.

---

## Post-Installation

You are ready to render now. You can create a new job in the MidRender Monitor application or install the optional DCC submitters.


### DCC Submitters

Browse to the shared folder you entered into the settings panel.

#### Adobe After Effects

In `plugins/afterEffects` is `MidRender.jsx`. Install this in your Adobe After Effects `Scripts/ScriptsUI Panels` folder. Press the `Scan Render Queue` to load all active Render Queue items. Set options and press `Submit`. 

![Mid Render Image](docs/images/AfterFX_FcOV6mcLiw.png)


> [!NOTE] 
> Chunk size will be honored for all image sequences, but not video outputs. If you are outputting a video, the plugin will automatically set the chunk size to the video's duration so that a single video is rendered (and not multiple video chunks).

#### Blender

To install the Blender submitter, use `Install from disk` in the `Add-ons` setting panel. The plugin will appear in the Render panels.

The MidRender submitter will automatically collect your render output frame range and output settings from the app, but you can toggle and adjust them for export. Press `Submit to Farm`.

![Mid Render Image](docs/images/blender_RQADDXgy9f.png)

#### Cinema 4D

To install the C4d submitter script, browse to your `%appdata%\Maxon\C4dDirectory\library\scripts` folder and copy `MidRender.py` to that location. Run it by selecting MidRender in the `Extensions > User Scripts` menu.

It will pull render paths and frame ranges from your user settings, but you can alter them. Press `Submit to Farm` when ready.

![Mid Render Image](docs/images/Cinema_4D_0cGq7BI1CC.png)

> [!NOTE]
> I only have one C4d license right now, so I have only been able to test this on one machine. It should work fine distributed on the farm though (famous last words).

## Mimimize to tray

When you close the app, it will minimize to the system tray. Running Mid Render minimized on your nodes is reccomended--it disengages window drawing. It releases resources to a minimal state, keeping only basic communication, coordination, and a lightweight Rust agent to manage CMD processes.

![Mid Render Image](docs/images/explorer_dM0xlpmQs3.png)

---

## Job Template structure

Job Templates are explained [here](docs/job-templates.md).
# AutoDaveSave
**By Dave Stewart**

## License
Apache License 2.0 (see [LICENSE](LICENSE))

## Repository
[https://github.com/netwebdave/AutoDaveSave](https://github.com/netwebdave/AutoDaveSave)

## Features
* **Silent Save All** at selected interval.
* **Menu checkmarks** show active interval and debug state.
* **Debug window** shows countdown to next autosave.

## How to Use
1.  Go to **Plugins > AutoDaveSave > Start or Stop Autosave**.
2.  Select interval: **1**, **3**, or **10** minutes.
3.  **Optional:** Select **Show Timer Selection (Debug)** for countdown.

## Notes
> * Untitled tabs can trigger "Save As" prompts when "Save All" runs.
> * Save files at least once manually to ensure silent autosave functions correctly.

## Installation

### Option 1: Secure Approach (Source Code)
*Recommended for environments meeting government and commercial cybersecurity requirements.*

1.  **Scan:** Analyze the provided `.cpp` file using your preferred source code scanners to verify security.
2.  **Build:** Compile the source code into a DLL using your preferred builder.
3.  **Install:**
    * Ensure Notepad++ is completely closed.
    * Navigate to `C:\Program Files\Notepad++\plugins`.
    * Create a new folder named `AutoDaveSave`.
    * Place your newly compiled `.dll` file inside this folder.

### Option 2: Standard Approach (Pre-built DLL)
1.  Ensure Notepad++ is completely closed.
2.  Navigate to `C:\Program Files\Notepad++\plugins`.
3.  Create a new folder named `AutoDaveSave`.
4.  Place the provided `.dll` file inside this folder.

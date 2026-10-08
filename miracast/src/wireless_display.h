// Internal: state of the Windows optional feature 「無線顯示器」 (Wireless
// Display, capability App.WirelessDisplay.Connect, CBS package
// Microsoft-Windows-WirelessDisplay-FOD-Package).  Windows 11 ships the
// Miracast *receiver* only with that feature; without it MiracastReceiver
// reports WiFiStatus = MiracastNotSupported even on capable hardware.
#pragma once

namespace pm::miracast {

enum class FeatureState { Unknown, NotInstalled, PendingReboot, Installed };

// Reads the CBS package state from HKLM (no admin rights needed, ~5-20 ms).
FeatureState wirelessDisplayFeatureState();

}  // namespace pm::miracast

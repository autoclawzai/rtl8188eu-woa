# Realtek rtwlanu 1030.9.0303.2016 (TP-Link TL-WN722N v3 installer): INF defaults for 2357:010C

Section: TplinkDisTxPwrTrain_RTL8188eu.ndi (Win10 x64 INF). Extracted from the TP-Link installer (NSIS -> InstallShield cab).

```
[UsbCommon.reg]
HKR,,LedCtrl,0,"1"
HKR,,QoS,0,"1"
HKR,,CcxRm,0,"1"
HKR,,CcxOffLineDurUpLimit,0,"0"
HKR,,PDNMode,0,"0"
HKR,,DongleSS,0,"0"
HKR,,SSPwrLvl,0,"2"
HKR,,AcUsbDmaTime,0,"10"
HKR,,AcUsbDmaSize,0,"5"
HKR,,AcUsbDmaTime2,0,"32"
HKR,,AcUsbDmaSize2,0,"5"
HKR,,FWOffload,0,"2"
HKR,,UsbRxAggBlockCount,0,"8"
HKR,,UsbRxAggBlockTimeout,0,"6"
HKR,,UsbRxAggPageTimeout,0,"6"
HKR,,UsbTxAggMode,0,"1"
HKR,,UsbTxAggDescNum,0,"6"
[Ndis6UsbRxAgg.reg]
HKR,,UsbRxAggMode,0,"2"
HKR,,UsbRxAggPageCount,0,"16"
[11nWirelessMode.reg]
HKR,,WirelessMode, 0, "8"
HKR,,BWSetting,0,"1"
HKR,,Channel,0,"10"
[TxByTimerOnn.reg]
HKR,,SendPacketByTimer,0,"1"
[DisableTxPowerTraining.reg]
HKR,,TxPowerTraining,0,"0"
[RTLWLAN.reg, selected]
HKR,,RxReorder,0,"1"
HKR,,RegRxReorder_WinSize,0,"128"
HKR,,RxReorder_PendTime,0,"100"
HKR,,bLeisurePs,0,"0"
HKR,,bFwCtrlLPS,0,"1"
HKR,,USBResetTxHang,0,"1"
HKR,,AddbaReqRetry,0,"0"
HKR,,MultiMode,0,"1"
HKR,,TxMode,0,"0"
HKR,,DynamicBatchEnable,0,"1"
```

Takeaways: USB TX aggregation (UsbTxAggMode=1, 6 descriptors per bulk transfer), SendPacketByTimer=1, BWSetting=20/40 MHz, RX reorder window 128, LPS off.

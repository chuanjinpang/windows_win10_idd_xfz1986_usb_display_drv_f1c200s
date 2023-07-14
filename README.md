# windows_win10_idd_xfz1986_usb_display_drv_f1c200s
win10 idd driver for usb display. support f1c200s, esp32s2

note: currently have a fatal issue about hardware decode jpg data, there are about 1% frame decode error which cause the screem flash.
it should releate USB transfer & hardware decode.  when USB doesn't work, the issue gone.

![image](https://github.com/chuanjinpang/windows_win10_idd_xfz1986_usb_display_drv_f1c200s/tree/master/demo/decode_err.jpg)

重要说明:当前有一个严重的问题， 硬解jpg数据时会有大概1%的概率会出错，生成屏幕闪烁一下。
这应该是和USB传输相关，当USB不工作时，没有发现硬解出错的问题。

## f1c200s demo
![image](https://github.com/chuanjinpang/windows_win10_idd_xfz1986_usb_display_drv_f1c200s/tree/master/demo/desktop1.jpg)
![image](https://github.com/chuanjinpang/windows_win10_idd_xfz1986_usb_display_drv_f1c200s/tree/master/demo/logon.jpg)

## esp32s2 demo
![image](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display/blob/main/demo/all.jpg)
![image](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display/blob/main/demo/esp32s2.jpg)
![image](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display/blob/main/demo/drv.png)
![image](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display/blob/main/demo/setting.png)

# windows_win10_idd_xfz1986_usb_display_drv_f1c200s
win10 idd driver for usb display. support f1c200s, esp32s2

note: currently have a fatal issue about hardware decode jpg data, there are about 1% frame decode error which cause the screem flash like below photo.
it should releate USB transfer & hardware decode.  when USB doesn't work, the issue gone.

![image](https://github.com/chuanjinpang/windows_win10_idd_xfz1986_usb_display_drv_f1c200s/blob/9fbfd85ad74e3fab1d8634d4ff97ee07aecba59c/demo/decode_err.jpg)

重要说明:当前有一个严重的问题， 硬解jpg数据时会有大概1%的概率会出错，生成屏幕闪烁一下，如上图。
这应该是和USB传输相关，当USB不工作时，没有发现硬解出错的问题。

解决方案：
1.找全志支持解决。
2.workaround 使用直接传输rgb565, VGA 640*480 能30fps.

## f1c200s demo

![image](https://github.com/chuanjinpang/windows_win10_idd_xfz1986_usb_display_drv_f1c200s/blob/9fbfd85ad74e3fab1d8634d4ff97ee07aecba59c/demo/logon.jpg))
![image](https://github.com/chuanjinpang/windows_win10_idd_xfz1986_usb_display_drv_f1c200s/blob/9fbfd85ad74e3fab1d8634d4ff97ee07aecba59c/demo/desktop1.jpg)


## esp32s2 demo
![image](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display/blob/main/demo/all.jpg)
![image](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display/blob/main/demo/esp32s2.jpg)
![image](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display/blob/main/demo/drv.png)
![image](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display/blob/main/demo/setting.png)

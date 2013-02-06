package org.uiautomation.iosdriver;

import java.util.List;

public class DeviceManager {

  static {
    System.loadLibrary("devicemanager");
  }

  public native String[] getDeviceList();
  public native String getDeviceInfo(String uuid);


  public static void main(String[] args) {
    DeviceManager manager = new DeviceManager();
    for (String s : manager.getDeviceList()){
      System.out.println("found device : "+s);
    }
    System.out.println("device info : "+manager.getDeviceInfo("d1ce6333af579e27d166349dc8a1989503ba5b4f"));
  }

}

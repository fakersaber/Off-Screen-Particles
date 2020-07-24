# Off-Screen-Particles

### Result



<video src="assets/HUAWEI_META20.mp4"></video>


#### Ground truth

![image-20200723155349846](assets/Ground_truth.png)



#### BilinearDownSample

![image-20200723155338794](assets/BilinearDowmSample.png)



#### NearestDownSample

![image-20200723160345299](assets/NearestDownSample.png)



### How To Use It

- 对于要离屏渲染的材质勾选**bDownSampleSeparateTranslucency**
- 确认Engine中开启**r.Mobile.SeparateTranslucency**



### #TODO

- [ ] 添加材质类型，即**EMeshPass**类型。

- [ ] 其他Blend状态修复（如Addtive）
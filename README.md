# Off-Screen-Particles

​	Render the Particles to a half-resolusion texture and blend to full resolution according to the depth distribution. If the interpolated depth is closer to the depth of the point sample, the color of the point sample is filled,  otherwise the color of the linear sample is filled.



## Effective Compare

#### GroudTruth

![GroudTruth](assets/GroudTruth.jpg)



### UE_Effective

In the case of DepthFade, there is aliasing due to direct sampling of SceneColor

![SampleSceneColor](assets/SampleSceneColor.jpg)



### My_Effective

#### DepthLoad

Try to use the original depth texture directly when using DepthFade

![DepthLoad](assets/DepthLoad.jpg)



#### FrameFetch

![DepthFrameFetch](assets/DepthFrameFetch.jpg)

## Optimization Result

#### Before：

![image-Before](assets/image-Before.png)

#### After：

![image-After](assets/image-After.png)

<video src="assets/HUAWEI_META20.mp4"></video>
## How To Use It

- 对于要离屏渲染的材质勾选**bDownSampleSeparateTranslucency**![image-20200729163613071](assets/Material_Editor.png)
- 确认Engine中开启**r.Mobile.SeparateTranslucency**



## TIPS

- 低端机本来Shading消耗就比较少，不太适用这个技术，比较适合于中端手机。
- 改进UE算生Nearest算法，改善Artifact
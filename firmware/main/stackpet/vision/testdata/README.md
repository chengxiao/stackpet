# 人脸检测基准的测试图

`face_probe.jpg`（320x240）是 `face_bench` 的正样本探针，只在 `-DSTACKPET_FD_BENCH=1` 的测试版里嵌进固件。

- 来源：Grace Hopper 的美国海军官方肖像（美国政府作品，属于公有领域），取自 matplotlib 自带的 `mpl-data/sample_data/grace_hopper.jpg`（512x600）。
- 处理：从 y=40 处裁出 512x384，再缩放到 320x240，JPEG 质量 80（macOS `sips`）。
- 人脸大约在 `[105, 70, 225, 185]`（x0, y0, x1, y1，像素）。基准要求每一帧检出的最佳人脸和这个框的 IoU ≥ 0.4。

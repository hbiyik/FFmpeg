# PLEASE USE https://github.com/nyanmisaka/ffmpeg-rockchip REPO INSTEAD.

# WHY?

Because the intention of this repo was to fix every shortcoming of rockchip VPU/Kernel/GPU software ecosystem in ffmpeg, even though it is not FFmpeg's responsibility.

In this sense, by design, the work here has 0 respect of FFmpeg conventions, and targets to break/fork only ffmpeg instead of other sw components like mesa/librarga/mpv/kodi etc. 1 fork was better than 5 different forks.

And it kinda worked. Espcially in this https://github.com/hbiyik/FFmpeg/tree/exp_refactor_all branch, there is a blazing fast decoder implementation of rockchip mpp.

But note that, it is still a decoder in ffmpeg library but not a proper ffmpeg-decoder, a lot of hacky and wonky stuff exists there.

By time passes it served its purpose, and the maintenance burden became heavy since it is reinventing lots of existing wheels.

# FFMPEG-ROCKCHIP

https://github.com/nyanmisaka/ffmpeg-rockchip on the other hand is a clean ffmpeg decoder / encoder / rga filter implementation, equally fast if not faster as `exp_refactor_all` branch.

To have a future proof implementation and combine the community effort including myself i suggest every interested party including myself to contribute to the new repo and new implementation at https://github.com/nyanmisaka/ffmpeg-rockchip

# WHATS HERE TO SEE

I think there are very interesting experiments in several branches in this repo for future reference. Therefore, i am not deleting the repo, just archiving it for future reference.

# HONORABLE MENTION

If you are interested in technical gory details in rockchip VPUs rga silicon, this issue https://github.com/JeffyCN/FFmpeg/issues/18 has one of richest resource on the internet about that, excluding our rant during our painful development process.

# SPECIAL THANKS

@JeffyCN
@rigaya
@nyanmisaka
@avafinger
@kwankiu
@kyak

and lots of others...

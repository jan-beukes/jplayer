# JPlayer
A simple video player made with [raylib](https://www.raylib.com/) and the [ffmpeg](https://ffmpeg.org/about.html)
libraries 

## Build
**Dependencies**
- ffmpeg / libav
- raylib
- yt-dlp (optional)

```
git clone --recurse-submodules https://github.com/jan-beukes/jplayer.git
cd jplayer
make BUILD_RAYLIB=TRUE VENDOR_FFMPEG=FALSE
```


## Usage
```
jplay <video file/url>
```
If yt-dlp is in PATH
```
jplay [-- OPTIONS] <youtube link>
```

# Image Carousel {#v4d_image_carousel}

@prev_tutorial{v4d_beauty}
@next_tutorial{v4d_imshow_reimplementation}

|    |    |
| -: | :- |
| Original author | Amir Hassan (kallaballa) <amir@viel-zu.org> |
| Compatibility | OpenCV >= 4.7 |

An interactive image gallery that loads images from files and directories and displays them as an animated carousel with glossy cards, reflections and smooth transitions. Navigate with the arrow keys, mouse scroll or mouse clicks, toggle auto-play with space, and tune the animation with the ImGui HUD. Demonstrates runtime image upload to NanoVG (`createImageRGBA`) and sharing a single mutable state object between the rendering pipeline and the GUI thread.

\htmlinclude "../samples/example_v4d_image_carousel.html"

@include samples/image_carousel.cpp
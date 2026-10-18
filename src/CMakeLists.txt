# core: pure model / commands / IO (no Qt, no FFmpeg)
file(GLOB CORE_SRC CONFIGURE_DEPENDS core/*.cpp core/*.h)
add_library(cutline_core STATIC ${CORE_SRC})
target_link_libraries(cutline_core PUBLIC thirdparty)

# render: D3D11 device, compositor, shaders, text
cutline_shader(layer.hlsl vs_main vs_5_0 layer_vs)
cutline_shader(layer.hlsl ps_main ps_5_0 layer_ps)
cutline_shader(transition.hlsl vs_main vs_5_0 transition_vs)
cutline_shader(transition.hlsl ps_main ps_5_0 transition_ps)
cutline_shader(present.hlsl vs_blit vs_5_0 present_blit_vs)
cutline_shader(present.hlsl ps_blit ps_5_0 present_blit_ps)
cutline_shader(present.hlsl vs_rect vs_5_0 present_rect_vs)
cutline_shader(present.hlsl ps_rect ps_5_0 present_rect_ps)
cutline_shader(present.hlsl ps_luma ps_5_0 present_luma_ps)
cutline_shader(present.hlsl ps_chroma ps_5_0 present_chroma_ps)
add_custom_target(cutline_shaders DEPENDS ${CUTLINE_SHADER_HEADERS})
file(GLOB RENDER_SRC CONFIGURE_DEPENDS render/*.cpp render/*.h)
add_library(cutline_render STATIC ${RENDER_SRC})
add_dependencies(cutline_render cutline_shaders)
target_link_libraries(cutline_render PUBLIC cutline_core Qt6::Gui d3d11 dxgi dxguid)

# media: probe / decode (FFmpeg)
file(GLOB MEDIA_SRC CONFIGURE_DEPENDS media/*.cpp media/*.h)
add_library(cutline_media STATIC ${MEDIA_SRC})
target_link_libraries(cutline_media PUBLIC cutline_render ffmpeg)

# audio: device output, mixer
file(GLOB AUDIO_SRC CONFIGURE_DEPENDS audio/*.cpp audio/*.h)
add_library(cutline_audio STATIC ${AUDIO_SRC})
target_link_libraries(cutline_audio PUBLIC cutline_media Qt6::Multimedia avrt ole32)

# engine: playback, sources, job pool
file(GLOB ENGINE_SRC CONFIGURE_DEPENDS engine/*.cpp engine/*.h)
if(ENGINE_SRC)
    add_library(cutline_engine STATIC ${ENGINE_SRC})
    target_link_libraries(cutline_engine PUBLIC cutline_audio cutline_media cutline_render)
else()
    add_library(cutline_engine INTERFACE)
endif()

# export: encoders, muxing
file(GLOB EXPORT_SRC CONFIGURE_DEPENDS export/*.cpp export/*.h)
if(EXPORT_SRC)
    add_library(cutline_export STATIC ${EXPORT_SRC})
    target_link_libraries(cutline_export PUBLIC cutline_engine cutline_media)
else()
    add_library(cutline_export INTERFACE)
endif()

# ui
file(GLOB UI_SRC CONFIGURE_DEPENDS ui/*.cpp ui/*.h)
add_library(cutline_ui STATIC ${UI_SRC})
target_link_libraries(cutline_ui PUBLIC cutline_export cutline_engine Qt6::Widgets Qt6::Svg)
# the version lives only in the top-level project(): the exe's file properties, About box and log use it
target_compile_definitions(cutline_ui PUBLIC CUTLINE_VERSION="${PROJECT_VERSION}"
    CUTLINE_VERSION_RC=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
# Help > User Guide shows the README
qt_add_resources(cutline_ui "guide" PREFIX "/guide" BASE ${CMAKE_SOURCE_DIR} FILES ${CMAKE_SOURCE_DIR}/README.md)

# app
add_executable(cutline WIN32 app/main.cpp app/cutline.rc)
target_link_libraries(cutline PRIVATE cutline_ui)

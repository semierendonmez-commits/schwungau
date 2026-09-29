# Schwung shell as a plugin. Two products share one code base:
#   Schwung     - instrument              (AU aumu, AUv3, VST3 Instrument, Standalone)
#   Schwung FX  - MIDI-controlled effect  (AU aumf, AUv3, VST3 Fx)
# The FX flavour takes audio input, which reaches modules the way it does on
# Move: through the SPI mailbox's audio-in block (linein, vocoder, ducker ...).

# JUCE 8.0.4: a checkout in third_party/JUCE if present, otherwise fetched.
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/JUCE ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

# AUv3 is an app extension: it must be signed with entitlements and is only
# practical with the full Xcode app, so it is opt-in (-DSCHWUNG_AUV3=ON,
# AUV3=1 tools/build_mac.sh). AU v2, VST3 and Standalone build with the
# Command Line Tools alone.
option(SCHWUNG_AUV3 "Also build the AUv3 app extension (needs Xcode)" OFF)
if(APPLE)
    set(SCHWUNG_FORMATS AU VST3 Standalone)
    if(SCHWUNG_AUV3)
        list(APPEND SCHWUNG_FORMATS AUv3)
    endif()
else()
    set(SCHWUNG_FORMATS VST3 Standalone)
endif()

juce_add_binary_data(schwung_fonts SOURCES
    Resources/Tamzen6x12r.bdf
    Resources/Tamzen8x16r.bdf)

set(SCHWUNG_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/BitmapFont.cpp
    Source/ModuleInstaller.cpp)

# Where the staged module tree lives (tools/stage_data.sh). Copied into each
# bundle's Resources/SchwungData so a fresh install is self-contained.
set(SCHWUNG_STAGED_DATA "${CMAKE_BINARY_DIR}/SchwungData" CACHE PATH "Staged Schwung data tree")

function(schwung_plugin target product code is_synth)
    if(is_synth)
        set(au_type kAudioUnitType_MusicDevice)
        set(vst3_cat Instrument)
        set(fx_flag 0)
    else()
        set(au_type kAudioUnitType_MusicEffect)
        set(vst3_cat Fx)
        set(fx_flag 1)
    endif()

    juce_add_plugin(${target}
        COMPANY_NAME "Schwung"
        COMPANY_WEBSITE "https://schwung.dev"
        BUNDLE_ID "dev.schwung.${target}"
        PLUGIN_MANUFACTURER_CODE Schw
        PLUGIN_CODE ${code}
        PRODUCT_NAME "${product}"
        VERSION ${PROJECT_VERSION}
        FORMATS ${SCHWUNG_FORMATS}
        IS_SYNTH ${is_synth}
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
        AU_MAIN_TYPE ${au_type}
        # NOT sandbox-safe: modules are dlopen()ed from the user's data
        # folder (~/Library/Application Support/Schwung), outside any host's
        # sandbox. Sandboxed hosts then run the AU out-of-process instead of
        # failing. The AUv3 carries its own copy of the modules in its
        # container (ModuleInstaller), so it needs no exception.
        AU_SANDBOX_SAFE FALSE
        HARDENED_RUNTIME_ENABLED TRUE
        # Modules are dlopen()'d from the user's data folder and are not signed
        # by us, so library validation must be off for this process.
        HARDENED_RUNTIME_OPTIONS com.apple.security.cs.disable-library-validation
        VST3_CATEGORIES ${vst3_cat}
        MICROPHONE_PERMISSION_ENABLED TRUE
        NEEDS_CURL FALSE
        NEEDS_WEB_BROWSER FALSE)

    target_sources(${target} PRIVATE ${SCHWUNG_SOURCES})
    target_compile_definitions(${target} PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        SCHWUNG_IS_FX=${fx_flag})
    target_link_libraries(${target}
        PRIVATE schwung_core schwung_fonts juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # Ship the module tree inside every bundle format.
    foreach(fmt IN LISTS SCHWUNG_FORMATS)
        set(ft ${target}_${fmt})
        if(TARGET ${ft} AND EXISTS "${SCHWUNG_STAGED_DATA}")
            get_target_property(artefact ${ft} JUCE_PLUGIN_ARTEFACT_FILE)
            if(fmt STREQUAL "Standalone" AND NOT APPLE)
                set(res_dir "$<TARGET_FILE_DIR:${ft}>/SchwungData")
            else()
                set(res_dir "${artefact}/Contents/Resources/SchwungData")
            endif()
            add_custom_command(TARGET ${ft} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${SCHWUNG_STAGED_DATA}" "${res_dir}"
                COMMENT "Bundling Schwung modules into ${ft}")
        endif()
    endforeach()
endfunction()

schwung_plugin(Schwung   "Schwung"    Swng TRUE)
schwung_plugin(SchwungFX "Schwung FX" Swfx FALSE)

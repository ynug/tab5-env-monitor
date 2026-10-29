set(VOICE ${CMAKE_CURRENT_LIST_DIR}/../../assets/voice)
idf_component_register(SRCS "audio.c"
                       INCLUDE_DIRS "include"
                       REQUIRES core
                       PRIV_REQUIRES esp_timer
                       EMBED_FILES ${VOICE}/co2_caution.wav ${VOICE}/co2_warning.wav ${VOICE}/heat_caution.wav
                                   ${VOICE}/heat_severe.wav ${VOICE}/heat_danger.wav ${VOICE}/humidity_low.wav
                                   ${VOICE}/humidity_high.wav ${VOICE}/temp_low.wav ${VOICE}/pressure_drop.wav)

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board accel neo7m_gps road_event pothole_model
)

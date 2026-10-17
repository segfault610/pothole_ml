idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board adxl345 neo7m_gps road_event pothole_model
)

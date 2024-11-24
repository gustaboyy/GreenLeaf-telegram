idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES hcf_adc
                    REQUIRES ioplaca
                    REQUIRES lcdvia595
                    REQUIRES mp_hcf
                    REQUIRES connect  
                    REQUIRES nvs_flash                  
                    REQUIRES mqtt
                    )
set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
    )


#ifndef _EI_CLASSIFIER_MODEL_VARIABLES_H_
#define _EI_CLASSIFIER_MODEL_VARIABLES_H_

#include <stdint.h>
#include "model_metadata.h"
#include "tflite-model/tflite_model_compiled_fast.h"
#include "tflite-model/tflite_model_compiled_balanced.h"
#include "tflite-model/tflite_model_compiled_accurate.h"
#include "edge-impulse-sdk/classifier/ei_model_types.h"
#include "edge-impulse-sdk/classifier/inferencing_engines/engines.h"
#include "edge-impulse-sdk/classifier/postprocessing/ei_postprocessing_common.h"

/* FAST */

const char* ei_classifier_inferencing_categories_fast[] = { "anamoly", "normal" };

EI_CLASSIFIER_DSP_AXES_INDEX_TYPE ei_dsp_config_fast_16_axes[] = { 0 };
const uint32_t ei_dsp_config_fast_16_axes_size = 1;
ei_dsp_config_flatten_t ei_dsp_config_fast_16 = {
    16, // uint32_t blockId
    1, // int implementationVersion
    1, // int length of axes
    1.0f, // float scale-axes
    true, // boolean average
    true, // boolean minimum
    true, // boolean maximum
    true, // boolean rms
    true, // boolean stdev
    true, // boolean skewness
    true, // boolean kurtosis
    0 // int moving_avg_num_windows
};

const float ei_dn_standard_scaler_mean_fast_16[7] = { 0.09547563486136587, 0.06740581272838415, 0.16095834116195384, 0.09981471636398286, 0.01578475547752413, 0.4311689896599036, 5.5415610403931135 };
const float ei_dn_standard_scaler_scale_fast_16[7] = { 43.21271863827187, 17.045271397227395, 5.631649217748478, 32.01844325250281, 31.036470720570538, 0.46343217286488136, 0.07601977137845638 };
const float ei_dn_standard_scaler_var_fast_16[7] = { 0.0005355213874362073, 0.0034418517406816333, 0.031530351219482555, 0.0009754377856415567, 0.0010381386015205017, 4.6561572481699685, 173.040149503144 };
ei_data_normalization_standard_scaler_config_t ei_data_normalization_standard_scaler_config_fast_16 = {
    .mean_data = (float *)ei_dn_standard_scaler_mean_fast_16,
    .mean_data_len = 7,
    .scale_data = (float *)ei_dn_standard_scaler_scale_fast_16,
    .scale_data_len = 7,
    .var_data = (float *)ei_dn_standard_scaler_var_fast_16,
    .var_data_len = 7
};
ei_data_normalization_t ei_data_normalization_config_fast_16 = {
    (void *) &ei_data_normalization_standard_scaler_config_fast_16, // config
    DATA_NORMALIZATION_METHOD_STANDARD_SCALER, // method
    nullptr, // context
    nullptr, // init func
    nullptr, // deinit func
    &data_normalization_standard_scaler // exec func
};

const uint8_t ei_dsp_blocks_fast_1_size = 1;
ei_model_dsp_t ei_dsp_blocks_fast_1[ei_dsp_blocks_fast_1_size] = {
    { // DSP block 16
        16,
        7, // output size
        &extract_flatten_features, // DSP function pointer
        (void*)&ei_dsp_config_fast_16, // pointer to config struct
        ei_dsp_config_fast_16_axes, // array of offsets into the input stream, one for each axis
        ei_dsp_config_fast_16_axes_size, // number of axes
        1, // version
        flatten_class::create, // factory function
        &ei_data_normalization_config_fast_16, // data normalization config
    }
};
const ei_config_tflite_eon_graph_t ei_config_graph_fast_10 = {
    .implementation_version = 1,
    .model_init = &tflite_learn_fast_init,
    .model_invoke = &tflite_learn_fast_invoke,
    .model_reset = &tflite_learn_fast_reset,
    .model_input = &tflite_learn_fast_input,
    .model_output = &tflite_learn_fast_output,
};

const uint8_t ei_output_tensors_indices_fast_10[1] = { 0 };
const uint8_t ei_output_tensors_size_fast_10 = 1;
ei_learning_block_config_tflite_graph_t ei_learning_block_config_fast_10 = {
    .implementation_version = 1,
    .block_id = 10,
    .output_tensors_indices = ei_output_tensors_indices_fast_10,
    .output_tensors_size = ei_output_tensors_size_fast_10,
    .quantized = 1,
    .compiled = 1,
    .graph_config = (void*)&ei_config_graph_fast_10,
    .dequantize_output = 0,
};

const uint8_t ei_learning_blocks_fast_1_size = 1;
const uint32_t ei_learning_block_fast_10_inputs[1] = { 16 };
const uint8_t ei_learning_block_fast_10_inputs_size = 1;
const ei_learning_block_t ei_learning_blocks_fast_1[ei_learning_blocks_fast_1_size] = {
    {
        10,
        &run_nn_inference,
        (void*)&ei_learning_block_config_fast_10,
        EI_CLASSIFIER_IMAGE_SCALING_NONE,
        ei_learning_block_fast_10_inputs,
        ei_learning_block_fast_10_inputs_size,
    },
};

ei_fill_result_classification_i8_config_t ei_fill_result_classification_i8_config_fast_10 = {
    .zero_point = -128,
    .scale = 0.00390625
};

const size_t ei_postprocessing_blocks_fast_1_size = 1;
const ei_postprocessing_block_t ei_postprocessing_blocks_fast_1[ei_postprocessing_blocks_fast_1_size] = {
    {
        .block_id = 10,
        .type = EI_CLASSIFIER_MODE_CLASSIFICATION,
        .init_fn = NULL,
        .deinit_fn = NULL,
        .postprocess_fn = &process_classification_i8,
        .display_fn = NULL,
        .config = (void*)&ei_fill_result_classification_i8_config_fast_10,
        .input_block_id = 10
    },
};

const uint8_t freeform_outputs_fast_1_size = 0;

uint32_t *freeform_outputs_fast_1 = nullptr;

const ei_impulse_t impulse_fast = {
    .project_id = 1076766,
    .project_owner = "Sachi207",
    .project_name = "ARIA-Demo1-ADC",
    .impulse_id = 1,
    .impulse_name = "Impulse #1",
    .deploy_version = 3,

    .nn_input_frame_size = 7,
    .raw_sample_count = 100,
    .raw_samples_per_frame = 1,
    .dsp_input_frame_size = 100 * 1,
    .input_width = 0,
    .input_height = 0,
    .input_frames = 0,
    .interval_ms = 100,
    .frequency = 10,

    .dsp_blocks_size = ei_dsp_blocks_fast_1_size,
    .dsp_blocks = ei_dsp_blocks_fast_1,

    .learning_blocks_size = ei_learning_blocks_fast_1_size,
    .learning_blocks = ei_learning_blocks_fast_1,

    .postprocessing_blocks_size = ei_postprocessing_blocks_fast_1_size,
    .postprocessing_blocks = ei_postprocessing_blocks_fast_1,

    .output_tensors_size = 1,

    .inferencing_engine = EI_CLASSIFIER_TFLITE,

    .sensor = EI_CLASSIFIER_SENSOR_FUSION,
    .fusion_string = "voltage",
    .slice_size = (100/4),
    .slices_per_model_window = 4,

    .has_anomaly = EI_ANOMALY_TYPE_UNKNOWN,
    .label_count = 2,
    .categories = ei_classifier_inferencing_categories_fast,
    .results_type = EI_CLASSIFIER_TYPE_CLASSIFICATION,
    .freeform_outputs_size = freeform_outputs_fast_1_size,
    .freeform_outputs = freeform_outputs_fast_1
};

ei_impulse_handle_t impulse_handle_fast = ei_impulse_handle_t( &impulse_fast );

ei_impulse_handle_t& ei_default_impulse = impulse_handle_fast;
/*
constexpr auto& ei_classifier_inferencing_categories = ei_classifier_inferencing_categories_fast;
const auto ei_dsp_blocks_size = ei_dsp_blocks_fast_1_size;
ei_model_dsp_t *ei_dsp_blocks = ei_dsp_blocks_fast_1;
*/

/* BALANCED */

const char* ei_classifier_inferencing_categories_balanced[] = { "anamoly", "normal" };

EI_CLASSIFIER_DSP_AXES_INDEX_TYPE ei_dsp_config_balanced_14_axes[] = { 0 };
const uint32_t ei_dsp_config_balanced_14_axes_size = 1;
ei_dsp_config_spectral_analysis_t ei_dsp_config_balanced_14 = {
    14, // uint32_t blockId
    4, // int implementationVersion
    1, // int length of axes
    1.0f, // float scale-axes
    1, // int input-decimation-ratio
    "none", // select filter-type
    3.0f, // float filter-cutoff
    6, // int filter-order
    "FFT", // select analysis-type
    16, // int fft-length
    3, // int spectral-peaks-count
    0.1f, // float spectral-peaks-threshold
    "0.1, 0.5, 1.0, 2.0, 5.0", // string spectral-power-edges
    true, // boolean do-log
    true, // boolean do-fft-overlap
    1, // int wavelet-level
    "db4", // select wavelet
    false // boolean extra-low-freq
};

EI_CLASSIFIER_DSP_AXES_INDEX_TYPE ei_dsp_config_balanced_16_axes[] = { 0 };
const uint32_t ei_dsp_config_balanced_16_axes_size = 1;
ei_dsp_config_flatten_t ei_dsp_config_balanced_16 = {
    16, // uint32_t blockId
    1, // int implementationVersion
    1, // int length of axes
    1.0f, // float scale-axes
    true, // boolean average
    true, // boolean minimum
    true, // boolean maximum
    true, // boolean rms
    true, // boolean stdev
    true, // boolean skewness
    true, // boolean kurtosis
    0 // int moving_avg_num_windows
};

const uint8_t ei_dsp_blocks_balanced_1_size = 2;
ei_model_dsp_t ei_dsp_blocks_balanced_1[ei_dsp_blocks_balanced_1_size] = {
    { // DSP block 14
        14,
        13, // output size
        &extract_spectral_analysis_features, // DSP function pointer
        (void*)&ei_dsp_config_balanced_14, // pointer to config struct
        ei_dsp_config_balanced_14_axes, // array of offsets into the input stream, one for each axis
        ei_dsp_config_balanced_14_axes_size, // number of axes
        1, // version
        nullptr, // factory function
        nullptr, // data normalization config
    },
    { // DSP block 16
        16,
        7, // output size
        &extract_flatten_features, // DSP function pointer
        (void*)&ei_dsp_config_balanced_16, // pointer to config struct
        ei_dsp_config_balanced_16_axes, // array of offsets into the input stream, one for each axis
        ei_dsp_config_balanced_16_axes_size, // number of axes
        1, // version
        flatten_class::create, // factory function
        nullptr, // data normalization config
    }
};
const ei_config_tflite_eon_graph_t ei_config_graph_balanced_10 = {
    .implementation_version = 1,
    .model_init = &tflite_learn_balanced_init,
    .model_invoke = &tflite_learn_balanced_invoke,
    .model_reset = &tflite_learn_balanced_reset,
    .model_input = &tflite_learn_balanced_input,
    .model_output = &tflite_learn_balanced_output,
};

const uint8_t ei_output_tensors_indices_balanced_10[1] = { 0 };
const uint8_t ei_output_tensors_size_balanced_10 = 1;
ei_learning_block_config_tflite_graph_t ei_learning_block_config_balanced_10 = {
    .implementation_version = 1,
    .block_id = 10,
    .output_tensors_indices = ei_output_tensors_indices_balanced_10,
    .output_tensors_size = ei_output_tensors_size_balanced_10,
    .quantized = 1,
    .compiled = 1,
    .graph_config = (void*)&ei_config_graph_balanced_10,
    .dequantize_output = 0,
};

const uint8_t ei_learning_blocks_balanced_1_size = 1;
const uint32_t ei_learning_block_balanced_10_inputs[1] = { 14 };
const uint8_t ei_learning_block_balanced_10_inputs_size = 1;
const ei_learning_block_t ei_learning_blocks_balanced_1[ei_learning_blocks_balanced_1_size] = {
    {
        10,
        &run_nn_inference,
        (void*)&ei_learning_block_config_balanced_10,
        EI_CLASSIFIER_IMAGE_SCALING_NONE,
        ei_learning_block_balanced_10_inputs,
        ei_learning_block_balanced_10_inputs_size,
    },
};

ei_fill_result_classification_i8_config_t ei_fill_result_classification_i8_config_balanced_10 = {
    .zero_point = -128,
    .scale = 0.00390625
};

const size_t ei_postprocessing_blocks_balanced_1_size = 1;
const ei_postprocessing_block_t ei_postprocessing_blocks_balanced_1[ei_postprocessing_blocks_balanced_1_size] = {
    {
        .block_id = 10,
        .type = EI_CLASSIFIER_MODE_CLASSIFICATION,
        .init_fn = NULL,
        .deinit_fn = NULL,
        .postprocess_fn = &process_classification_i8,
        .display_fn = NULL,
        .config = (void*)&ei_fill_result_classification_i8_config_balanced_10,
        .input_block_id = 10
    },
};

const uint8_t freeform_outputs_balanced_1_size = 0;

uint32_t *freeform_outputs_balanced_1 = nullptr;

const ei_impulse_t impulse_balanced = {
    .project_id = 1076766,
    .project_owner = "Sachi207",
    .project_name = "ARIA-Demo1-ADC",
    .impulse_id = 1,
    .impulse_name = "Impulse #1",
    .deploy_version = 2,

    .nn_input_frame_size = 13,
    .raw_sample_count = 200,
    .raw_samples_per_frame = 1,
    .dsp_input_frame_size = 200 * 1,
    .input_width = 0,
    .input_height = 0,
    .input_frames = 0,
    .interval_ms = 100,
    .frequency = 10,

    .dsp_blocks_size = ei_dsp_blocks_balanced_1_size,
    .dsp_blocks = ei_dsp_blocks_balanced_1,

    .learning_blocks_size = ei_learning_blocks_balanced_1_size,
    .learning_blocks = ei_learning_blocks_balanced_1,

    .postprocessing_blocks_size = ei_postprocessing_blocks_balanced_1_size,
    .postprocessing_blocks = ei_postprocessing_blocks_balanced_1,

    .output_tensors_size = 1,

    .inferencing_engine = EI_CLASSIFIER_TFLITE,

    .sensor = EI_CLASSIFIER_SENSOR_FUSION,
    .fusion_string = "voltage",
    .slice_size = (200/4),
    .slices_per_model_window = 4,

    .has_anomaly = EI_ANOMALY_TYPE_UNKNOWN,
    .label_count = 2,
    .categories = ei_classifier_inferencing_categories_balanced,
    .results_type = EI_CLASSIFIER_TYPE_CLASSIFICATION,
    .freeform_outputs_size = freeform_outputs_balanced_1_size,
    .freeform_outputs = freeform_outputs_balanced_1
};

ei_impulse_handle_t impulse_handle_balanced = ei_impulse_handle_t( &impulse_balanced );
/*
ei_impulse_handle_t& ei_default_impulse = impulse_handle_balanced;
constexpr auto& ei_classifier_inferencing_categories = ei_classifier_inferencing_categories_balanced;
const auto ei_dsp_blocks_size = ei_dsp_blocks_balanced_1_size;
ei_model_dsp_t *ei_dsp_blocks = ei_dsp_blocks_balanced_1;
*/

/* ACCURATE */

const char* ei_classifier_inferencing_categories_accurate[] = { "anamoly", "normal" };

EI_CLASSIFIER_DSP_AXES_INDEX_TYPE ei_dsp_config_accurate_16_axes[] = { 0 };
const uint32_t ei_dsp_config_accurate_16_axes_size = 1;
ei_dsp_config_flatten_t ei_dsp_config_accurate_16 = {
    16, // uint32_t blockId
    1, // int implementationVersion
    1, // int length of axes
    1.0f, // float scale-axes
    true, // boolean average
    true, // boolean minimum
    true, // boolean maximum
    true, // boolean rms
    true, // boolean stdev
    true, // boolean skewness
    true, // boolean kurtosis
    0 // int moving_avg_num_windows
};

EI_CLASSIFIER_DSP_AXES_INDEX_TYPE ei_dsp_config_accurate_17_axes[] = { 0 };
const uint32_t ei_dsp_config_accurate_17_axes_size = 1;
ei_dsp_config_spectral_analysis_t ei_dsp_config_accurate_17 = {
    17, // uint32_t blockId
    4, // int implementationVersion
    1, // int length of axes
    1.0f, // float scale-axes
    1, // int input-decimation-ratio
    "none", // select filter-type
    3.0f, // float filter-cutoff
    6, // int filter-order
    "FFT", // select analysis-type
    16, // int fft-length
    3, // int spectral-peaks-count
    0.1f, // float spectral-peaks-threshold
    "0.1, 0.5, 1.0, 2.0, 5.0", // string spectral-power-edges
    true, // boolean do-log
    true, // boolean do-fft-overlap
    1, // int wavelet-level
    "db4", // select wavelet
    false // boolean extra-low-freq
};

const float ei_dn_standard_scaler_mean_accurate_16[7] = { 0.09456010093152499, 0.04572591850464975, 0.20266291549778068, 0.09927511784055393, 0.01918336366066389, 0.5308825614841316, 14.75832588717961 };
const float ei_dn_standard_scaler_scale_accurate_16[7] = { 49.901351393846255, 11.49636589632876, 4.506683301523897, 41.52454547618213, 37.156974162380386, 0.28904408149307426, 0.023432907760387717 };
const float ei_dn_standard_scaler_var_accurate_16[7] = { 0.00040158306115968084, 0.007566217903524781, 0.04923635792344703, 0.0005799495625624931, 0.0007243013906984885, 11.969385035466837, 1821.1580337555306 };
ei_data_normalization_standard_scaler_config_t ei_data_normalization_standard_scaler_config_accurate_16 = {
    .mean_data = (float *)ei_dn_standard_scaler_mean_accurate_16,
    .mean_data_len = 7,
    .scale_data = (float *)ei_dn_standard_scaler_scale_accurate_16,
    .scale_data_len = 7,
    .var_data = (float *)ei_dn_standard_scaler_var_accurate_16,
    .var_data_len = 7
};
ei_data_normalization_t ei_data_normalization_config_accurate_16 = {
    (void *) &ei_data_normalization_standard_scaler_config_accurate_16, // config
    DATA_NORMALIZATION_METHOD_STANDARD_SCALER, // method
    nullptr, // context
    nullptr, // init func
    nullptr, // deinit func
    &data_normalization_standard_scaler // exec func
};

const uint8_t ei_dsp_blocks_accurate_1_size = 2;
ei_model_dsp_t ei_dsp_blocks_accurate_1[ei_dsp_blocks_accurate_1_size] = {
    { // DSP block 16
        16,
        7, // output size
        &extract_flatten_features, // DSP function pointer
        (void*)&ei_dsp_config_accurate_16, // pointer to config struct
        ei_dsp_config_accurate_16_axes, // array of offsets into the input stream, one for each axis
        ei_dsp_config_accurate_16_axes_size, // number of axes
        1, // version
        flatten_class::create, // factory function
        &ei_data_normalization_config_accurate_16, // data normalization config
    },
    { // DSP block 17
        17,
        13, // output size
        &extract_spectral_analysis_features, // DSP function pointer
        (void*)&ei_dsp_config_accurate_17, // pointer to config struct
        ei_dsp_config_accurate_17_axes, // array of offsets into the input stream, one for each axis
        ei_dsp_config_accurate_17_axes_size, // number of axes
        1, // version
        nullptr, // factory function
        nullptr, // data normalization config
    }
};
const ei_config_tflite_eon_graph_t ei_config_graph_accurate_10 = {
    .implementation_version = 1,
    .model_init = &tflite_learn_accurate_init,
    .model_invoke = &tflite_learn_accurate_invoke,
    .model_reset = &tflite_learn_accurate_reset,
    .model_input = &tflite_learn_accurate_input,
    .model_output = &tflite_learn_accurate_output,
};

const uint8_t ei_output_tensors_indices_accurate_10[1] = { 0 };
const uint8_t ei_output_tensors_size_accurate_10 = 1;
ei_learning_block_config_tflite_graph_t ei_learning_block_config_accurate_10 = {
    .implementation_version = 1,
    .block_id = 10,
    .output_tensors_indices = ei_output_tensors_indices_accurate_10,
    .output_tensors_size = ei_output_tensors_size_accurate_10,
    .quantized = 1,
    .compiled = 1,
    .graph_config = (void*)&ei_config_graph_accurate_10,
    .dequantize_output = 0,
};

const uint8_t ei_learning_blocks_accurate_1_size = 1;
const uint32_t ei_learning_block_accurate_10_inputs[1] = { 16 };
const uint8_t ei_learning_block_accurate_10_inputs_size = 1;
const ei_learning_block_t ei_learning_blocks_accurate_1[ei_learning_blocks_accurate_1_size] = {
    {
        10,
        &run_nn_inference,
        (void*)&ei_learning_block_config_accurate_10,
        EI_CLASSIFIER_IMAGE_SCALING_NONE,
        ei_learning_block_accurate_10_inputs,
        ei_learning_block_accurate_10_inputs_size,
    },
};

ei_fill_result_classification_i8_config_t ei_fill_result_classification_i8_config_accurate_10 = {
    .zero_point = -128,
    .scale = 0.00390625
};

const size_t ei_postprocessing_blocks_accurate_1_size = 1;
const ei_postprocessing_block_t ei_postprocessing_blocks_accurate_1[ei_postprocessing_blocks_accurate_1_size] = {
    {
        .block_id = 10,
        .type = EI_CLASSIFIER_MODE_CLASSIFICATION,
        .init_fn = NULL,
        .deinit_fn = NULL,
        .postprocess_fn = &process_classification_i8,
        .display_fn = NULL,
        .config = (void*)&ei_fill_result_classification_i8_config_accurate_10,
        .input_block_id = 10
    },
};

const uint8_t freeform_outputs_accurate_1_size = 0;

uint32_t *freeform_outputs_accurate_1 = nullptr;

const ei_impulse_t impulse_accurate = {
    .project_id = 1076766,
    .project_owner = "Sachi207",
    .project_name = "ARIA-Demo1-ADC",
    .impulse_id = 1,
    .impulse_name = "Impulse #1",
    .deploy_version = 4,

    .nn_input_frame_size = 7,
    .raw_sample_count = 300,
    .raw_samples_per_frame = 1,
    .dsp_input_frame_size = 300 * 1,
    .input_width = 0,
    .input_height = 0,
    .input_frames = 0,
    .interval_ms = 100,
    .frequency = 10,

    .dsp_blocks_size = ei_dsp_blocks_accurate_1_size,
    .dsp_blocks = ei_dsp_blocks_accurate_1,

    .learning_blocks_size = ei_learning_blocks_accurate_1_size,
    .learning_blocks = ei_learning_blocks_accurate_1,

    .postprocessing_blocks_size = ei_postprocessing_blocks_accurate_1_size,
    .postprocessing_blocks = ei_postprocessing_blocks_accurate_1,

    .output_tensors_size = 1,

    .inferencing_engine = EI_CLASSIFIER_TFLITE,

    .sensor = EI_CLASSIFIER_SENSOR_FUSION,
    .fusion_string = "voltage",
    .slice_size = (300/4),
    .slices_per_model_window = 4,

    .has_anomaly = EI_ANOMALY_TYPE_UNKNOWN,
    .label_count = 2,
    .categories = ei_classifier_inferencing_categories_accurate,
    .results_type = EI_CLASSIFIER_TYPE_CLASSIFICATION,
    .freeform_outputs_size = freeform_outputs_accurate_1_size,
    .freeform_outputs = freeform_outputs_accurate_1
};

ei_impulse_handle_t impulse_handle_accurate = ei_impulse_handle_t( &impulse_accurate );
/*
ei_impulse_handle_t& ei_default_impulse = impulse_handle_accurate;
constexpr auto& ei_classifier_inferencing_categories = ei_classifier_inferencing_categories_accurate;
const auto ei_dsp_blocks_size = ei_dsp_blocks_accurate_1_size;
ei_model_dsp_t *ei_dsp_blocks = ei_dsp_blocks_accurate_1;
*/
#pragma once
// SOSD DATASET CONNECTION SETTINGS
// Open this file in Visual Studio (Header Files > DatasetConfig.h),
// then set the two absolute paths below to the existing binary SOSD files.
// The DATASETS ARE NOT COPIED into the project and are NOT compiled.
// Use forward slashes to avoid escaping backslashes in C++ strings.
// Example: "C:/SOSD_Data/uniform_dense_200M_uint32"
namespace SosdConfig {
inline constexpr const char* DENSE_PATH  = "C:/SOSD_Data/uniform_dense_200M_uint32";
inline constexpr const char* SPARSE_PATH = "C:/SOSD_Data/uniform_sparse_200M_uint32";
}

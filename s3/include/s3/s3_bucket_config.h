#ifndef S3_BUCKET_CONFIG_H
#define S3_BUCKET_CONFIG_H

#include <s3/s3.h>

#ifdef __cplusplus
extern "C" {
#endif

int s3_get_bucket_lifecycle(s3_client *client, const char *bucket, s3_response *out_response,
                            s3_error *out_error);
int s3_put_bucket_lifecycle(s3_client *client, const char *bucket, const void *xml, size_t xml_size,
                            s3_response *out_response, s3_error *out_error);
int s3_delete_bucket_lifecycle(s3_client *client, const char *bucket, s3_response *out_response,
                               s3_error *out_error);

int s3_get_bucket_notification(s3_client *client, const char *bucket, s3_response *out_response,
                               s3_error *out_error);
int s3_put_bucket_notification(s3_client *client, const char *bucket, const void *xml,
                               size_t xml_size, s3_response *out_response, s3_error *out_error);

int s3_get_bucket_replication(s3_client *client, const char *bucket, s3_response *out_response,
                              s3_error *out_error);
int s3_put_bucket_replication(s3_client *client, const char *bucket, const void *xml,
                              size_t xml_size, s3_response *out_response, s3_error *out_error);
int s3_delete_bucket_replication(s3_client *client, const char *bucket, s3_response *out_response,
                                 s3_error *out_error);

#ifdef __cplusplus
}
#endif

#endif /* S3_BUCKET_CONFIG_H */

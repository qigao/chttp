#ifndef S3_BUCKET_H
#define S3_BUCKET_H

#include <s3/s3.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s3_bucket_info {
  char *name;
  char *creation_date;
} s3_bucket_info;

typedef struct s3_bucket_list {
  s3_bucket_info *items;
  size_t count;
} s3_bucket_list;

int s3_create_bucket(s3_client *client, const char *bucket, s3_response *out_response,
                     s3_error *out_error);
int s3_head_bucket(s3_client *client, const char *bucket, s3_response *out_response,
                   s3_error *out_error);
int s3_delete_bucket(s3_client *client, const char *bucket, s3_response *out_response,
                     s3_error *out_error);
int s3_list_buckets(s3_client *client, s3_bucket_list *out_list, s3_response *out_response,
                    s3_error *out_error);
void s3_bucket_list_destroy(s3_bucket_list *list);

#ifdef __cplusplus
}
#endif

#endif /* S3_BUCKET_H */

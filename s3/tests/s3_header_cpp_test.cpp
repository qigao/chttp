#include <s3/s3.h>
#include <s3/s3_bucket.h>
#include <s3/s3_bucket_config.h>
#include <s3/s3_credentials.h>
#include <s3/s3_multipart.h>
#include <s3/s3_object.h>
#include <s3/s3_signer.h>

int main() {
  s3_client client{};
  s3_async_client async_client{};
  s3_response response{};
  s3_bucket_list buckets{};
  s3_object_list objects{};
  s3_multipart multipart{};
  s3_multipart_file_options multipart_options{};
  s3_signer_result signature{};
  s3_response_destroy(&response);
  s3_bucket_list_destroy(&buckets);
  s3_object_list_destroy(&objects);
  if (s3_multipart_destroy(&multipart) != SALTS_OK) return 1;
  s3_signer_result_destroy(&signature);
  return client.impl == nullptr && async_client.impl == nullptr && multipart_options.size == 0u &&
                 S3_MULTIPART_MAX_PARTS == 10000
             ? 0
             : 1;
}

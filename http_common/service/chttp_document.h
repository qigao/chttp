#ifndef CHTTP_DOCUMENT_H
#define CHTTP_DOCUMENT_H

#include <data_bind_method_plan.h>
#include <data_bind_native.h>
#include <data_bind_message_plan.h>

/* Private transport bridge. Plans/providers are borrowed until mount teardown;
 * writers and native field views are invocation-local. No wire codec lives here. */
typedef struct chttp_document_plan {
  const DataBindFormatPlan *format_plan;
  DataBindFormat format;
  const char *root_name;
} chttp_document_plan;

typedef struct chttp_document_writer {
  DataBindFormatCanonicalWriter canonical;
  DataBindFormatCursor cursor;
  cserde_writer bounded;
  const DataBindNativeOptions *options;
  size_t remaining_bytes;
  size_t remaining_tokens;
} chttp_document_writer;

int chttp_document_admit(const DataBindTransportPlan *transport,
    const DataBindBindingPlan *binding, const char *space,
    int allow_xml, chttp_document_plan *out);
int chttp_document_format_admit(const DataBindFormatPlan *plan,
    const DataBindBindingPlan *binding, const char *space,
    int allow_xml, chttp_document_plan *out);
int chttp_document_message_admit(const DataBindMessagePlan *message,
    const DataBindNativeTypeBinding *expected,
    const chttp_document_plan *document, size_t field_count);
int chttp_document_input_admit(const DataBindTransportPlan *transport,
    const DataBindBindingPlan *binding, chttp_document_plan *out);
DataBindStatus chttp_document_begin(chttp_document_writer *document,
    const chttp_document_plan *plan, cserde_writer *target,
    const DataBindNativeOptions *options, size_t capacity, DataBindError *error);
DataBindStatus chttp_document_field(chttp_document_writer *document,
    const DataBindBindingPlanEntry *entry, DataBindBindingValueState state,
    const void *value, size_t bytes, DataBindError *error);
DataBindStatus chttp_document_finish(chttp_document_writer *document);

#endif

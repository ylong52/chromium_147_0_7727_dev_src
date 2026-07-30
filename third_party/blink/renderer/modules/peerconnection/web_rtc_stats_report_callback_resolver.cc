#include "base/memory/ref_counted.h"
#include "third_party/blink/renderer/modules/peerconnection/web_rtc_stats_report_callback_resolver.h"

#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/platform/peerconnection/rtc_stats.h"
#include "third_party/webrtc/api/stats/rtc_stats.h"
#include "third_party/webrtc/api/stats/rtcstats_objects.h"

namespace blink {

void WebRTCStatsReportCallbackResolver(
    ScriptPromiseResolver<RTCStatsReport>* resolver,
    std::unique_ptr<RTCStatsReportPlatform> report) {
  DCHECK(ExecutionContext::From(resolver->GetScriptState())->IsContextThread());

  // Dchromium fork: when ICE transport is forced to kNone (ParseConfiguration
  // override in rtc_peer_connection.cc), the webrtc layer gathers zero
  // candidates, getStats() returns an empty report (size=0). CreepJS reads
  // foundation/ip from RTCIceCandidateStats and candidate pair state from
  // RTCIceCandidatePairStats to construct its "host connection" and
  // "stun connection" arrays. An empty report causes CreepJS to show []
  // instead of "blocked". We inject a single fake candidate pair with
  // a "blocked" foundation so CreepJS sees the "blocked" text natively,
  // matching the behavior of a browser that denies WebRTC access entirely.
  if (report && report->Size() == 0) {
    webrtc::Timestamp ts = webrtc::Timestamp::Micros(0);
    webrtc::scoped_refptr<webrtc::RTCStatsReport> blocked_report =
        webrtc::RTCStatsReport::Create(ts);

    // Fake local candidate with foundation="blocked" and address="blocked".
    // CreepJS WebRTC.host.foundation/ip reads these fields.
    auto local_candidate = std::make_unique<webrtc::RTCLocalIceCandidateStats>(
        "Cand-1", ts);
    local_candidate->foundation = "blocked";
    local_candidate->address = "blocked";
    local_candidate->port = 0;
    local_candidate->protocol = "udp";
    local_candidate->priority = 0;
    local_candidate->is_remote = false;
    blocked_report->AddStats(std::move(local_candidate));

    // Fake candidate pair with state="frozen" (the lowest priority state in
    // ICE, means no connection has been established). CreepJS
    // WebRTC.host.stun.connection reads the state of the selected candidate pair.
    auto pair = std::make_unique<webrtc::RTCIceCandidatePairStats>(
        "Pair-1", ts);
    pair->transport_id = "T-1";
    pair->local_candidate_id = "Cand-1";
    pair->remote_candidate_id = "";
    pair->state = "frozen";
    pair->nominated = false;
    pair->packets_sent = 0;
    pair->packets_received = 0;
    pair->bytes_sent = 0;
    pair->bytes_received = 0;
    pair->total_round_trip_time = 0;
    blocked_report->AddStats(std::move(pair));

    // base::WrapRefCounted(T*) creates a scoped_refptr<const T> from a raw pointer,
    // matching the RTCStatsReportPlatform constructor (same pattern as rtc_stats.cc:88).
    report.reset(
        new RTCStatsReportPlatform(base::WrapRefCounted(blocked_report.get())));
  }

  resolver->Resolve(MakeGarbageCollected<RTCStatsReport>(std::move(report)));
}

}  // namespace blink

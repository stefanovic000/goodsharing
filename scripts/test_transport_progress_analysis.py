"""Offline controls for interval counts and stream/reset separation."""
from analyze_transport_progress import intervals


def sample(at, ssrc, packets, frames=0):
    return dict(local_us=at, tracks=[dict(kind='video', ssrc=ssrc, packets_sent=packets, frames_sent=frames)])


rows = [sample(100, 1, 10, 4), sample(200, 1, 15, 7), sample(200, 2, 999, 100),
        sample(300, 1, 2, 1), sample(400, 2, 1001, 101)]
result = intervals(rows, ('packets_sent', 'frames_sent'))
assert len(result) == 3 and result[0]['deltas'] == dict(packets_sent=5, frames_sent=3)
assert not result[0]['counter_reset'] and result[1]['counter_reset']
assert result[2]['ssrc'] == 2 and result[2]['deltas']['packets_sent'] == 2
assert intervals([sample(200, 1, 10), sample(100, 1, 11)], ('packets_sent',)) == []
assert intervals([], ('packets_sent',)) == []
print('PASS: RTP interval deltas, separate SSRCs, reset markers, nonpositive duration and missing evidence')

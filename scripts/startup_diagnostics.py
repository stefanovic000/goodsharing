"""Interpret test-only native receiver setup events; no media or key logging."""
import re


def analyze_startup(log, stall_ms=0):
    receivers={}
    for stage,receiver,stamp in re.findall(r'GoodSharing startup ([a-z-]+) receiver=([0-9A-Fa-f]+) at_us=(\d+)',log):
        receivers.setdefault(receiver,[]).append({'stage':stage,'at_us':int(stamp)})
    # SDP also announces dormant receivers which never become a room track.
    # Their early-only guards must stay closed; only claimed room tracks need
    # a later activation. Counting them as missing activations is incorrect.
    claimed={receiver:events for receiver,events in receivers.items() if any(event['stage']!='early-blocked' for event in events)}
    dormant={receiver:events for receiver,events in receivers.items() if receiver not in claimed}
    videos=[]
    for receiver,events in claimed.items():
        stages={event['stage']:event['at_us'] for event in events}
        metadata=stages.get('metadata-detached',stages.get('metadata-only'))
        active=stages.get('crypto-active',stages.get('crypto-chain'))
        if metadata is not None:
            videos.append({'receiver':receiver,'events':events,
                           'metadata_to_activation_us':active-metadata if active is not None else None,
                           'closed_order':all(key in stages for key in ('early-blocked','blocked','metadata-detached','crypto-active')) and stages['early-blocked']<=stages['blocked']<=stages['metadata-detached']<=stages['crypto-active']})
    no_intermediate=not any(event['stage'] in ('metadata-only','cryptor-only') for events in receivers.values() for event in events)
    closed=bool(claimed) and no_intermediate and len(videos)==2 and all(video['closed_order'] for video in videos)
    closed &= all([event['stage'] for event in events].count('early-blocked')==1 and [event['stage'] for event in events].count('blocked')==1 and [event['stage'] for event in events].count('crypto-active')==1 for events in claimed.values())
    fault=len(videos)==2 and all(video['metadata_to_activation_us'] is not None and video['metadata_to_activation_us']>=stall_ms*1000 for video in videos)
    warnings=[line for line in log.splitlines() if line.startswith('[h264 @ ')]
    return {'closed_pipeline_order_verified':bool(closed),'requested_stall_ms':stall_ms,
            'setup_stall_verified':bool(fault),'receiver_count':len(claimed),
            'early_guarded_receiver_count':sum(any(event['stage']=='early-blocked' for event in events) for events in receivers.values()),
            'dormant_early_receiver_count':len(dormant),
            'video_setup':videos,'decoder_warning_count':len(warnings),'decoder_warnings':warnings[:20]}

"""A Windows atomic snapshot sharing conflict must remain pending."""
from pathlib import Path
from unittest.mock import patch
from test_remote_av import checkpoint_count

with patch.object(Path, 'read_text', side_effect=[FileNotFoundError(), PermissionError(13, 'sharing conflict'), '[{}, {}]']):
    assert checkpoint_count(Path('owned-checkpoints.json')) == 0
    assert checkpoint_count(Path('owned-checkpoints.json')) == 0
    assert checkpoint_count(Path('owned-checkpoints.json')) == 2
with patch.object(Path, 'read_text', return_value='broken'):
    try:
        checkpoint_count(Path('owned-checkpoints.json'))
        raise AssertionError('Malformed snapshot was accepted')
    except ValueError:
        pass
print('PASS: missing/replaced snapshots stay pending; next complete snapshot is readable; malformed data is not accepted; caller deadlines unchanged')

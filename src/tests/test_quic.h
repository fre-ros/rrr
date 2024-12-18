/*

Read Route Record

Copyright (C) 2022-2024 Atle Solbakken atle@goliathdns.no

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef RRR_TEST_QUIC_H
#define RRR_TEST_QUIC_H

struct rrr_event_queue;

int rrr_test_quic (const volatile int *main_running, struct rrr_event_queue *queue);

#endif /* RRR_TEST_QUIC_H */

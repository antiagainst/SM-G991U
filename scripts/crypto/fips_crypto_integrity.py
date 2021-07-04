#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
This script is needed for buildtime integrity routine.
It calculates and embeds HMAC and other needed stuff for in terms of FIPS 140-2
"""
import os
import sys
from IntegrityRoutine import IntegrityRoutine
from Utils import Utils

__author__ = "Vadym Stupakov"
__copyright__ = "Copyright (c) 2017 Samsung Electronics"
__credits__ = ["Vadym Stupakov"]
__version__ = "1.0"
__maintainer__ = "Vadym Stupakov"
__email__ = "v.stupakov@samsung.com"
__status__ = "Production"

list_obj_files_skc = [
    "fips140_integrity.o",
    "fips140_post.o",
    "fips140_test.o",
    "fips140_test_tv.o",
    "ghash-generic.o",
    "api.o",
    "cipher.o",
    "compress.o",
    "memneq.o",
    "proc.o",
    "algapi.o",
    "scatterwalk.o",
    "aead.o",
    "ablkcipher.o",
    "blkcipher.o",
    "skcipher.o",
    "seqiv.o",
    "echainiv.o",
    "ahash.o",
    "shash.o",
    "algboss.o",
    "testmgr.o",
    "hmac.o",
    "sha1_generic.o",
    "sha256_generic.o",
    "sha512_generic.o",
    "ecb.o",
    "cbc.o",
    "ctr.o",
    "gcm.o",
    "aes_generic.o",
    "authenc.o",
    "authencesn.o",
    "rng.o",
    "drbg.o",
    "jitterentropy.o",
    "jitterentropy-kcapi.o",
    "../lib/crypto/aes.o",
    "../lib/crypto/sha256.o"
]

list_obj_files_skc_ce = [
    "aes-ce-core.o",
    "aes-ce-glue.o",
    "aes-ce.o",
    "aes-cipher-core.o",
    "aes-cipher-glue.o",
    "aes-glue-ce.o",
    "sha256-core.o",
    "sha256-glue.o",
    "sha2-ce-core.o",
    "sha2-ce-glue.o",
    "sha1-ce-glue.o",
    "sha1-ce-core.o"
]

def find_first_obj_file(path_to_obj_files):
    file_obj_name = None
    for directory_file_list in path_to_obj_files:
        path_to_files = directory_file_list[0]
        for l_file in directory_file_list[1]:
            if os.path.isfile(os.path.join(path_to_files, l_file)):
                file_obj_name = os.path.join(path_to_files, l_file)
                break
        if file_obj_name is not None:
            break
    return file_obj_name

module_name = "crypto"

if __name__ == "__main__":

    if len(sys.argv) != 4:
        print("Usage {} [elf_file] [path to SKC *.o files] [path to SKC-CE *.o files]".format(sys.argv[0]))
        sys.exit(-1)

    print("module_name: ", module_name)

    elf_file = os.path.abspath(sys.argv[1])
    relative_path_to_skc_obj = sys.argv[2]
    relative_path_to_skc_ce_obj = sys.argv[3]

    utils = Utils()
    utils.paths_exists([elf_file])

    obj_files_full_path = [
                           [relative_path_to_skc_obj, list_obj_files_skc],
                           [relative_path_to_skc_ce_obj, list_obj_files_skc_ce]
                          ]

    first_obj_file = find_first_obj_file(obj_files_full_path)

    if first_obj_file is not None:
        integrity = IntegrityRoutine(elf_file, first_obj_file)
        sec_sym = integrity.get_filtered_canister_symbols(obj_files_full_path, debug=True)
        integrity.make_integrity(sec_sym=sec_sym, module_name=module_name, debug=False, print_reloc_gaps=False)
    else:
        print("ERROR: no OBJs files for parsing")
        sys.exit(-1)

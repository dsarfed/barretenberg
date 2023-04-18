#include "barretenberg/transcript/transcript.hpp"
#include "barretenberg/plonk/proof_system/proving_key/serialize.hpp"
#include "barretenberg/stdlib/primitives/curves/bn254.hpp"
#include "barretenberg/stdlib/recursion/verifier/verifier.hpp"
#include "barretenberg/stdlib/recursion/verifier/program_settings.hpp"
#include "barretenberg/ecc/curves/bn254/fq12.hpp"
#include "barretenberg/ecc/curves/bn254/pairing.hpp"

//#include <transcript/transcript.hpp>
//#include <proof_system/proving_key/serialize.hpp>
//#include <stdlib/primitives/curves/bn254.hpp>
//#include <stdlib/recursion/verifier/verifier.hpp>
//#include <stdlib/recursion/verifier/program_settings.hpp>
//#include <ecc/curves/bn254/fq12.hpp>
//#include <ecc/curves/bn254/pairing.hpp>

using namespace proof_system::plonk;

using numeric::uint256_t;

template <typename OuterComposer> class RecursiveCircuit {
    using InnerComposer = UltraComposer;

    typedef stdlib::field_t<InnerComposer> field_ct;
    typedef stdlib::bn254<InnerComposer> inner_curve;
    typedef stdlib::bn254<OuterComposer> outer_curve;
    typedef stdlib::recursion::verification_key<outer_curve> verification_key_pt;
    typedef stdlib::recursion::recursive_ultra_verifier_settings<outer_curve> recursive_settings;
    typedef stdlib::recursion::recursive_ultra_to_standard_verifier_settings<outer_curve>
        ultra_to_standard_recursive_settings;
    typedef inner_curve::fr_ct fr_ct;
    typedef inner_curve::public_witness_ct public_witness_ct;
    typedef inner_curve::witness_ct witness_ct;

    struct circuit_outputs {
        stdlib::recursion::aggregation_state<outer_curve> aggregation_state;
        std::shared_ptr<verification_key_pt> verification_key;
    };

    static void create_inner_circuit_no_tables(InnerComposer& composer, uint256_t inputs[])
    {
        field_ct a(witness_ct(&composer, inputs[0]));
        field_ct b(public_witness_ct(&composer, inputs[1]));
        field_ct c(public_witness_ct(&composer, inputs[2]));

        // @note For some reason, if we don't have this line, the circuit fails to verify.
        // auto c_sq = c * c;

        c.assert_equal(a + b);
    };

    static circuit_outputs create_outer_circuit(InnerComposer& inner_composer, OuterComposer& outer_composer)
    {
        // These constexpr definitions are to allow for the following:
        // An Ultra Pedersen hash evaluates to a different value from the Turbo/Standard versions of the Pedersen hash.
        // Therefore, the fiat-shamir challenges generated by the prover and verifier _could_ accidentally be different
        // if an ultra proof is generated using ultra-pedersen challenges, but is being verified within a non-ultra
        // circuit which uses non-ultra-pedersen challenges. We need the prover and verifier hashes to be the same. The
        // solution is to select the relevant prover and verifier types (whose settings use the same hash for
        // fiat-shamir), depending on the Inner-Outer combo. It's a bit clunky, but the alternative is to have a
        // template argument for the hashtype, and that would pervade the entire UltraComposer, which would be
        // horrendous.
        constexpr bool is_ultra_to_ultra = std::is_same<OuterComposer, UltraComposer>::value;
        typedef
            typename std::conditional<is_ultra_to_ultra, UltraProver, UltraToStandardProver>::type ProverOfInnerCircuit;
        typedef typename std::conditional<is_ultra_to_ultra, UltraVerifier, UltraToStandardVerifier>::type
            VerifierOfInnerProof;
        typedef
            typename std::conditional<is_ultra_to_ultra, recursive_settings, ultra_to_standard_recursive_settings>::type
                RecursiveSettings;

        ProverOfInnerCircuit prover;
        if constexpr (is_ultra_to_ultra) {
            prover = inner_composer.create_prover();
        } else {
            prover = inner_composer.create_ultra_to_standard_prover();
        }

        const auto verification_key_native = inner_composer.compute_verification_key();

        // Convert the verification key's elements into _circuit_ types, using the OUTER composer.
        std::shared_ptr<verification_key_pt> verification_key =
            verification_key_pt::from_witness(&outer_composer, verification_key_native);

        proof recursive_proof = prover.construct_proof();

        {
            // Native check is mainly for comparison vs circuit version of the verifier.
            VerifierOfInnerProof native_verifier;

            if constexpr (is_ultra_to_ultra) {
                native_verifier = inner_composer.create_verifier();
            } else {
                native_verifier = inner_composer.create_ultra_to_standard_verifier();
            }
            auto native_result = native_verifier.verify_proof(recursive_proof);
            if (native_result == false) {
                throw std::runtime_error("Native verification failed");
            }
        }

        transcript::Manifest recursive_manifest = InnerComposer::create_manifest(prover.key->num_public_inputs);

        // Verifying the ultra (inner) proof with CIRCUIT TYPES (i.e. within a standard or ultra plonk arithmetic
        // circuit)
        stdlib::recursion::aggregation_state<outer_curve> output =
            stdlib::recursion::verify_proof<outer_curve, RecursiveSettings>(
                &outer_composer, verification_key, recursive_manifest, recursive_proof);

        return { output, verification_key };
    };

  public:
    static OuterComposer generate(std::string srs_path, uint256_t inputs[])
    {
        InnerComposer inner_composer = InnerComposer(srs_path);
        OuterComposer outer_composer = OuterComposer(srs_path);

        create_inner_circuit_no_tables(inner_composer, inputs);
        auto circuit_output = create_outer_circuit(inner_composer, outer_composer);

        g1::affine_element P[2];
        P[0].x = barretenberg::fq(circuit_output.aggregation_state.P0.x.get_value().lo);
        P[0].y = barretenberg::fq(circuit_output.aggregation_state.P0.y.get_value().lo);
        P[1].x = barretenberg::fq(circuit_output.aggregation_state.P1.x.get_value().lo);
        P[1].y = barretenberg::fq(circuit_output.aggregation_state.P1.y.get_value().lo);

        barretenberg::fq12 inner_proof_result = barretenberg::pairing::reduced_ate_pairing_batch_precomputed(
            P, circuit_output.verification_key->reference_string->get_precomputed_g2_lines(), 2);

        if (inner_proof_result != barretenberg::fq12::one()) {
            throw std::runtime_error("inner proof result != 1");
        }

        circuit_output.aggregation_state.add_proof_outputs_as_public_inputs();

        if (outer_composer.failed()) {
            throw std::runtime_error("outer composer failed");
        }

        return outer_composer;
    }
};
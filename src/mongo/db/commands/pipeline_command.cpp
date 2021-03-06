/**
 * Copyright (c) 2011-2014 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
namespace {

bool isMergePipeline(const std::vector<BSONObj>& pipeline) {
    if (pipeline.empty()) {
        return false;
    }
    return pipeline[0].hasField("$mergeCursors");
}

class PipelineCommand final : public Command {
public:
    PipelineCommand() : Command("aggregate") {}

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        // TODO: Parsing to a Pipeline and/or AggregationRequest here.
        return std::make_unique<Invocation>(this, opMsgRequest);
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(Command* cmd, const OpMsgRequest& request)
            : CommandInvocation(cmd),
              _request(request),
              _dbName(request.getDatabase().toString()) {}

    private:
        bool supportsWriteConcern() const override {
            return Pipeline::aggSupportsWriteConcern(this->_request.body);
        }

        bool supportsReadConcern(repl::ReadConcernLevel level) const override {
            // Aggregations that are run directly against a collection allow any read concern.
            // Otherwise, if the aggregate is collectionless then the read concern must be 'local'
            // (e.g. $currentOp). The exception to this is a $changeStream on a whole database,
            // which is
            // considered collectionless but must be read concern 'majority'. Further read concern
            // validation is done one the pipeline is parsed.
            return level == repl::ReadConcernLevel::kLocalReadConcern ||
                level == repl::ReadConcernLevel::kMajorityReadConcern ||
                !AggregationRequest::parseNs(_dbName, _request.body).isCollectionlessAggregateNS();
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            auto bob = reply->getBodyBuilder();
            const auto aggregationRequest = uassertStatusOK(
                AggregationRequest::parseFromBSON(_dbName, _request.body, boost::none));

            uassertStatusOK(runAggregate(opCtx,
                                         aggregationRequest.getNamespaceString(),
                                         aggregationRequest,
                                         _request.body,
                                         bob));
        }

        NamespaceString ns() const override {
            return AggregationRequest::parseNs(_dbName, _request.body);
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     BSONObjBuilder* out) override {
            const auto aggregationRequest = uassertStatusOK(
                AggregationRequest::parseFromBSON(_dbName, _request.body, verbosity));

            uassertStatusOK(runAggregate(opCtx,
                                         aggregationRequest.getNamespaceString(),
                                         aggregationRequest,
                                         _request.body,
                                         *out));
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            const auto nss = ns();
            uassertStatusOK(AuthorizationSession::get(opCtx->getClient())
                                ->checkAuthForAggregate(nss, _request.body, false));
        }

        const OpMsgRequest& _request;
        const std::string _dbName;
    };

    std::string help() const override {
        return "Runs the aggregation command. See http://dochub.mongodb.org/core/aggregation for "
               "more details.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

} pipelineCmd;

}  // namespace
}  // namespace mongo
